/**
 * mitm.c — 资源 MITM：改写 orpheus:// 页面的 HTML，把载荷"随页面发进去"
 *
 * 为什么走这条路（docs/07 §7.19–7.20）：
 *   实测渲染进程侧 frame 的 execute_java_script(idx18) 是**空操作**
 *   （连 location.href 导航都不发生），v8context::eval 也全部无效。
 *   而参照项目 BetterNCM 的 UI 注入根本不经过 JS 执行 API ——
 *   它钩 cef_register_scheme_handler_factory，把 orpheus:// 资源的
 *   read_response 换掉，先把原文抽干、再改写内容、最后吐回去。
 *   页面加载到的就是"已经带我们脚本"的 HTML，天然执行。
 *
 * 本文件的实现与 reference/v2-extracted/EasyCEFHooks.cpp 的
 * hook_cef_register_scheme_handler_factory / hook_cef_scheme_handler_create /
 * hook_scheme_handler_read 一一对应，但：
 *   - 用我们自己的 IAT 补丁（不依赖 Detour）
 *   - 只在 URL 命中 orpheus:// 且正文像 HTML 时才改写，其余原样透传
 *   - 每次改写都写 trace，便于实机核对
 */
#include "mitm.h"
#include "util.h"
#include "inject.h"
#include "plugins.h"
#include "cef_capi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* ── 结构体槽位（CEF 91 capi 顺序，用前都做行为/形态校验） ──────────────
 * cef_scheme_handler_factory_t : base; create
 * cef_resource_handler_t       : base; process_request; get_response_headers;
 *                                read_response; can_get_cookie; can_set_cookie;
 *                                skip_response_bytes; cancel
 * cef_request_t                : base; is_read_only; get_url; ...
 * 这里只用最保守的几个：factory->create 是第一个方法（0x28），
 * read_response 是第三个方法（0x28 + 2*8 = 0x38）。 */
#define NCM_BASE              0x28u
#define FACTORY_CREATE_OFF    (NCM_BASE + 0u)
#define RES_READ_RESPONSE_OFF (NCM_BASE + 2u * 8u)
#define RES_GET_HEADERS_OFF   (NCM_BASE + 1u * 8u)

typedef int  (*ncm_read_response_fn)(void *self, void *data_out, int bytes_to_read,
                                     int *bytes_read, void *callback);
typedef void *(*ncm_factory_create_fn)(void *self, void *browser, void *frame,
                                       const void *scheme_name, void *request);
typedef int  (*ncm_register_factory_fn)(const void *scheme_name, void *factory_2,
                                        void *factory_3);

static void *g_orig_reg_factory = NULL;
static volatile LONG g_create_calls = 0;
static volatile LONG g_read_calls = 0;
static void *g_orig_factory_create = NULL;

/* ── 每个 resource_handler 的改写状态 ───────────────────────────────── */
typedef struct {
    void        *handler;
    void        *orig_read;
    char         url[512];
    char        *buf;
    size_t       len;
    size_t       pos;
    int          filled;
} mitm_entry;

#define MITM_MAX 8
static mitm_entry g_e[MITM_MAX];

static mitm_entry *entry_find(void *handler)
{
    for (int i = 0; i < MITM_MAX; i++)
        if (g_e[i].handler == handler) return &g_e[i];
    return NULL;
}

static mitm_entry *entry_new(void *handler)
{
    mitm_entry *e = entry_find(handler);
    if (e) return e;
    for (int i = 0; i < MITM_MAX; i++) {
        if (!g_e[i].handler) {
            memset(&g_e[i], 0, sizeof(g_e[i]));
            g_e[i].handler = handler;
            return &g_e[i];
        }
    }
    return NULL;
}

static void entry_free(mitm_entry *e)
{
    if (!e) return;
    free(e->buf);
    memset(e, 0, sizeof(*e));
}

/* ── request->get_url：逐个字段试，能返回 orpheus:// 串的那个就是 ────── */
static int request_url(void *request, char *out, int out_size)
{
    if (!request) return 0;
    for (int idx = 0; idx <= 6; idx++) {
        uintptr_t fn = 0;
        __try { fn = *(uintptr_t *)((uint8_t *)request + NCM_BASE + (uintptr_t)idx * 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (!ncm_ptr_is_code(fn)) continue;

        cef_string_t *s = NULL;
        __try { s = ((cef_string_t *(*)(void *))fn)(request); }
        __except (EXCEPTION_EXECUTE_HANDLER) { s = NULL; }
        if (!s || !s->str || s->length == 0 || s->length > 4096) continue;

        char tmp[1024] = {0};
        if (ncm_cefstr_to_utf8(s, tmp, sizeof(tmp)) > 0 &&
            (strncmp(tmp, "orpheus://", 10) == 0 || strncmp(tmp, "http", 4) == 0)) {
            strncpy_s(out, out_size, tmp, _TRUNCATE);
            return 1;
        }
    }
    return 0;
}

/* ── 把载荷包成 <script>，插到 </body> 之前 ─────────────────────────── */
static char *inject_into_html(const char *body, size_t len, size_t *out_len)
{
    char *bundle = ncm_bundle_build();
    if (!bundle) return NULL;

    /* 载荷里不能出现 "</script"，否则会提前闭合标签（构建期已检查过，这里再兜一次） */
    for (char *p = bundle; (p = strstr(p, "</script")) != NULL; p += 8)
        *p = ' ';

    size_t blen = strlen(bundle);
    /* <script> + 载荷 + </script>，再加一点余量 */
    char *tag_open = "\n<script>/*ncm-inject: resource MITM*/;(function(){try{\n";
    char *tag_close = "\n}catch(e){try{console.error('[ncm-inject] payload error',e);}catch(_){}}})();</script>\n";
    size_t need = len + strlen(tag_open) + blen + strlen(tag_close) + 64;

    char *out = (char *)malloc(need);
    if (!out) { free(bundle); return NULL; }

    /* 找 </body>（大小写不敏感，简单实现：同时找 </body> 和 </BODY>） */
    size_t at = len;
    for (size_t i = 0; i + 7 <= len; i++) {
        if (_strnicmp(body + i, "</body>", 7) == 0) { at = i; break; }
    }

    memcpy(out, body, at);
    size_t off = at;
    memcpy(out + off, tag_open, strlen(tag_open));  off += strlen(tag_open);
    memcpy(out + off, bundle, blen);                off += blen;
    memcpy(out + off, tag_close, strlen(tag_close)); off += strlen(tag_close);
    if (at < len) {
        memcpy(out + off, body + at, len - at);
        off += len - at;
    }
    out[off] = 0;
    *out_len = off;

    ncm_trace("MITM：HTML 注入完成 原 %zu → 新 %zu 字节（载荷 %zu）", len, off, blen);
    free(bundle);
    return out;
}

/* ── read_response 包装：第一次读时把原文抽干并改写 ──────────────────── */
static int hook_read_response(void *self, void *data_out, int bytes_to_read,
                              int *bytes_read, void *callback)
{
    InterlockedIncrement(&g_read_calls);
    mitm_entry *e = entry_find(self);
    if (!e) {
        /* 不是我们接管的 handler：找它的原始实现直接转发 */
        return 0;
    }

    if (!e->filled) {
        ncm_read_response_fn orig = (ncm_read_response_fn)e->orig_read;
        if (!orig) { e->filled = 1; return 0; }

        /* 抽干原文（参照实现同名逻辑 fillData） */
        const int chunk = 65535;
        char *tmp = (char *)malloc(chunk);
        if (!tmp) { e->filled = 1; return 0; }

        size_t cap = 1 << 20, len = 0;
        char *buf = (char *)malloc(cap);
        int first = 1;
        if (!buf) { free(tmp); e->filled = 1; return 0; }

        for (;;) {
            int n = 0;
            int r = 0;
            __try { r = orig(self, tmp, chunk - 1, &n, NULL); }
            __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; n = 0; }
            if (r != 1) first = 0;
            if (n > 0) {
                if (len + (size_t)n + 1 > cap) {
                    size_t ncap = cap * 2;
                    while (ncap < len + (size_t)n + 1) ncap *= 2;
                    char *nb = (char *)realloc(buf, ncap);
                    if (!nb) break;
                    buf = nb; cap = ncap;
                }
                memcpy(buf + len, tmp, (size_t)n);
                len += (size_t)n;
            }
            if (r != 1 || n <= 0) break;
            if (len > (32u << 20)) break;   /* 32MB 上限，防失控 */
        }
        free(tmp);
        buf[len] = 0;

        int is_html = (len > 16) &&
                      (strstr(buf, "<html") || strstr(buf, "<!DOCTYPE") || strstr(buf, "<!doctype"));
        ncm_trace("MITM：抽干资源 %s（%zu 字节，%s）",
                  e->url, len, is_html ? "HTML" : "非 HTML");

        size_t newlen = len;
        char *nbuf = NULL;
        if (is_html) nbuf = inject_into_html(buf, len, &newlen);
        if (nbuf) { free(buf); buf = nbuf; len = newlen; }
        else if (is_html) {
            ncm_trace("MITM：改写失败，原样透传");
        }

        e->buf = buf;
        e->len = len;
        e->pos = 0;
        e->filled = 1;
        (void)first;
    }

    size_t remain = e->len - e->pos;
    size_t want = (bytes_to_read > 0) ? (size_t)bytes_to_read : 0;
    size_t n = remain < want ? remain : want;
    if (n > 0) {
        memcpy(data_out, e->buf + e->pos, n);
        e->pos += n;
        if (bytes_read) *bytes_read = (int)n;
        return 1;
    }
    if (bytes_read) *bytes_read = 0;
    /* 吐完就清掉这条记录（handler 可能被复用） */
    entry_free(e);
    return 0;
}

/* ── factory->create：拿到 handler 后接管它的 read_response ──────────── */
static void *hook_factory_create(void *self, void *browser, void *frame,
                                 const void *scheme_name, void *request)
{
    ncm_factory_create_fn orig = (ncm_factory_create_fn)g_orig_factory_create;
    if (!orig) return NULL;

    static volatile LONG calls = 0;
    InterlockedIncrement(&g_create_calls);
    LONG n0 = InterlockedIncrement(&calls);
    if (n0 <= 12) ncm_trace("MITM：create #%ld 被调用（self=%p browser=%p frame=%p）", n0, self, browser, frame);

    void *handler = NULL;
    __try { handler = orig(self, browser, frame, scheme_name, request); }
    __except (EXCEPTION_EXECUTE_HANDLER) { handler = NULL; }

    char url[512] = {0};
    int got_url = request_url(request, url, sizeof(url));

    LONG n = n0;
    if (n <= 12)
        ncm_trace("MITM：create #%ld → handler=%p url=%s（取到=%d）",
                  n, handler, got_url ? url : "(读不到)", got_url);
    if (!handler) return NULL;

    /* 只接管 orpheus:// 的资源；其余完全不动 */
    if (strncmp(url, "orpheus://", 10) != 0) return handler;

    mitm_entry *e = entry_new(handler);
    if (!e) return handler;

    uintptr_t read_fn = 0;
    __try { read_fn = *(uintptr_t *)((uint8_t *)handler + RES_READ_RESPONSE_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { read_fn = 0; }
    if (!ncm_ptr_is_code(read_fn)) {
        ncm_trace("MITM：handler=%p 的 read_response 槽位不像代码（0x%zX），放弃接管",
                  handler, RES_READ_RESPONSE_OFF);
        entry_free(e);
        return handler;
    }

    strncpy_s(e->url, sizeof(e->url), url, _TRUNCATE);
    e->orig_read = (void *)read_fn;

    __try {
        *(uintptr_t *)((uint8_t *)handler + RES_READ_RESPONSE_OFF) = (uintptr_t)hook_read_response;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ncm_trace("MITM：替换 read_response 抛异常，放弃");
        entry_free(e);
        return handler;
    }

    ncm_trace("MITM：已接管 %s（handler=%p，原 read_response=%p）",
              e->url, handler, (void *)read_fn);
    return handler;
}

/* ── 注册钩子：factory 结构体的 create 换成我们的 ────────────────────── */
int ncm_mitm_register_hook(const void *scheme_name, void *f2, void *f3)
{
    /* 这个导出在不同 CEF 版本上参数个数不同（2 个或 3 个）。
     * x64 下多传一个参数无害，所以这里按 3 参接收，再挑"像 factory 的那个"：
     * 合法 factory 在 +0 有 size、+0x28 是代码指针。 */
    void *cands[2] = { f2, f3 };
    void *factory = NULL;
    for (int i = 0; i < 2; i++) {
        void *f = cands[i];
        if (!f) continue;
        size_t sz = 0;
        __try { sz = *(size_t *)f; } __except (EXCEPTION_EXECUTE_HANDLER) { sz = 0; }
        if (sz < 0x28 || sz > 0x200) continue;
        uintptr_t create = 0;
        __try { create = *(uintptr_t *)((uint8_t *)f + FACTORY_CREATE_OFF); }
        __except (EXCEPTION_EXECUTE_HANDLER) { create = 0; }
        if (!ncm_ptr_is_code(create)) continue;
        factory = f;
        break;
    }

    if (factory) {
        char scheme[128] = "(?)";
        char dom[256] = "-";
        if (scheme_name) ncm_cefstr_to_utf8((const cef_string_t *)scheme_name, scheme, sizeof(scheme));
        /* f2 在某些 CEF 版本里是 domain 字符串 */
        if (f2 && f3) {
            const cef_string_t *cs = (const cef_string_t *)f2;
            if (cs->str && cs->length > 0 && cs->length < 200) ncm_cefstr_to_utf8(cs, dom, sizeof(dom));
        }
        ncm_trace("MITM：注册 scheme=\"%s\" domain=\"%s\" factory=%p", scheme, dom, factory);
        uintptr_t create = *(uintptr_t *)((uint8_t *)factory + FACTORY_CREATE_OFF);
        g_orig_factory_create = (void *)create;
        __try {
            *(uintptr_t *)((uint8_t *)factory + FACTORY_CREATE_OFF) = (uintptr_t)hook_factory_create;
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        ncm_trace("MITM：factory=%p 的 create 已接管（原 %p）", factory, (void *)create);
        /* 读后校验：确认写入没被 CEF 改回去 */
        uintptr_t now = 0;
        __try { now = *(uintptr_t *)((uint8_t *)factory + FACTORY_CREATE_OFF); }
        __except (EXCEPTION_EXECUTE_HANDLER) { now = 0; }
        ncm_trace("MITM：写后回读 create=%p（%s）", (void *)now,
                  now == (uintptr_t)hook_factory_create ? "仍是我们" : "已被改回/未生效");
    } else {
        ncm_trace("MITM：没认出 factory（f2=%p f3=%p），本次不接管", f2, f3);
    }

    ncm_register_factory_fn orig = (ncm_register_factory_fn)g_orig_reg_factory;
    if (!orig) return 0;
    return orig(scheme_name, f2, f3);
}
/* 供 dump_layout 读取：create/read 的真实调用次数（不依赖日志通道） */
long ncm_mitm_create_calls(void) { return (long)g_create_calls; }
long ncm_mitm_read_calls(void)   { return (long)g_read_calls; }