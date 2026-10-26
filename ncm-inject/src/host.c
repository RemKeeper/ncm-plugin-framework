/**
 * host.c — 页面 JS ⇄ 本 DLL 的原生桥（实现）
 *
 * 只用 CEF 的**公开 C API**：
 *   - cef_register_extension(name, code, handler)   ← 导出函数，IAT 里也有
 *   - cef_v8value_create_string(const cef_string_t*) ← 导出函数
 *   - 收到的 v8value 用结构体里的方法指针读字符串（槽位**自检**得出，不靠猜）
 *
 * 不猜偏移的地方：
 *   cef_v8handler_t 是**我们自己实现**的，布局由我们定（base + execute）。
 *   cef_v8value_t 是 CEF 造的，所以 get_string_value 的槽位必须先自检：
 *   我们自己造一个已知内容的字符串 v8value，然后逐个候选槽位调用，
 *   谁返回的 cef_string_t 正好等于那个已知内容，谁就是 get_string_value。
 *   每个候选都套 __try，猜错只会被吞掉，不会崩。
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cef_capi.h"
#include "host.h"
#include "util.h"
#include "plugins.h"

/* ── 用到的 libcef 导出（GetProcAddress 拿） ─────────────────────────────── */
typedef void *(*fn_create_string)(const cef_string_t *str);
typedef void  (*fn_register_extension)(const cef_string_t *name,
                                       const cef_string_t *code, void *handler);

static fn_create_string      g_create_string = NULL;
static fn_register_extension g_register_ext  = NULL;

static volatile LONG g_calls = 0;         /* JS→原生 调用次数 */
static volatile LONG g_ok    = 0;         /* 成功处理次数 */
static int g_string_idx      = -1;        /* get_string_value 的槽位 */
static int g_registered      = 0;

/* ── 延迟日志：V8 线程**绝不能**直接写日志文件 ────────────────────────────
 * 实测（docs/07 §7.2.5）：page JS 在 CEF V8 线程里回调进 DLL 后，
 * 只要内联走一次 ncm_log（加锁 + 时间格式化 + 文件写），整个应用 ~8 秒内退出；
 * 同样路径只返回字符串、不写日志则完全正常（mode=5 存活 24s+）。
 * ⇒ 原生桥只把字符串塞进无锁环，由 worker 心跳线程落盘。 */
#define NCM_PEND_N   96
#define NCM_PEND_SZ  512

static char          g_pend[NCM_PEND_N][NCM_PEND_SZ];
static volatile LONG g_pend_w    = 0;
static LONG          g_pend_l    = 0;
static volatile LONG g_pend_lost = 0;

static void pend_push(const char *s)
{
    LONG i = InterlockedIncrement(&g_pend_w) - 1;
    if (i >= NCM_PEND_N) { InterlockedIncrement(&g_pend_lost); return; }
    if (!s) s = "";
    size_t n = strlen(s);
    if (n > NCM_PEND_SZ - 1) n = NCM_PEND_SZ - 1;
    memcpy(g_pend[i], s, n);
    g_pend[i][n] = 0;
}

int ncm_host_drain(void)
{
    LONG w = g_pend_w;
    if (w > NCM_PEND_N) w = NCM_PEND_N;
    int n = 0;
    while (g_pend_l < w) {
        ncm_log("[页面 JS] %s", g_pend[g_pend_l]);
        g_pend_l++;
        n++;
    }
    if (g_pend_lost) {
        ncm_log("[页面 JS] 另有 %ld 条因队列满被丢弃", g_pend_lost);
        g_pend_lost = 0;
    }
    return n;
}

/* ── 同步 trace：崩溃前最后一条面包屑 ─────────────────────────────────────
 * 延迟队列（pend_push）要等 worker 线程排空，最短也要 100ms；
 * 而被注入的扩展如果**同一个 JS turn 内**就崩掉，队列里的东西一条都留不下。
 * 所以这里再开一条同步通道：直接用 WriteFile 追加到 trace-<pid>.log。
 * 只用 kernel32（CreateFileW/WriteFile），不碰 CRT 堆、不碰任何锁，
 * 因此从 CEF 的 V8 线程调用是安全的（与 mode=9/10 崩在 JS 侧是两回事）。 */
static HANDLE g_trace = INVALID_HANDLE_VALUE;
static wchar_t g_trace_path[MAX_PATH * 2] = {0};
static long long g_trace_bytes = 0;   /* 已写字节数，用于即时判断超限 */

static void trace_write(const char *s)
{
    if (g_trace == INVALID_HANDLE_VALUE) {
        const char *la = getenv("LOCALAPPDATA");
        if (!la || !*la) return;
        char p[MAX_PATH * 2];
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\NCMPlugin\\trace-%lu.log",
                    la, GetCurrentProcessId());
        wchar_t w[MAX_PATH * 2];
        if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w, MAX_PATH * 2) <= 0) return;
        wcsncpy_s(g_trace_path, _countof(g_trace_path), w, _TRUNCATE);
        g_trace = CreateFileW(w, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (g_trace == INVALID_HANDLE_VALUE || !s) return;
    DWORD wr = 0;
    WriteFile(g_trace, s, (DWORD)strlen(s), &wr, NULL);
    WriteFile(g_trace, "\r\n", 2, &wr, NULL);
    /* 体积上限：按累计字节即时判断，超了就轮转成 trace-<pid>.log.1 */
    g_trace_bytes += (long long)strlen(s) + 2;
    if (g_trace_bytes > ncm_log_max_bytes()) {
        CloseHandle(g_trace);
        g_trace = INVALID_HANDLE_VALUE;
        ncm_log_rotate(g_trace_path);
        g_trace = CreateFileW(g_trace_path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        g_trace_bytes = 0;
    }
}

void ncm_trace(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt ? fmt : "", ap);
    va_end(ap);
    trace_write(buf);      /* 同步，崩了也留得下 */
    pend_push(buf);        /* 正式日志由 worker 线程落盘 */
}

/* ── 引用计数：静态对象，永活 ───────────────────────────────────────────── */
static void h_add_ref(void *s) { (void)s; }
static int  h_release(void *s) { (void)s; return 0; }
static int  h_has_one(void *s) { (void)s; return 0; }
static int  h_has_at_least(void *s) { (void)s; return 1; }

/* ── ★ 我们自己的 cef_v8handler_t（布局由我们定义） ─────────────────────── */
typedef struct {
    ncm_cef_base_ref_counted base;
    int (*execute)(void *self, const cef_string_t *name, void *object,
                   size_t argc, void **argv, void **retval, cef_string_t *exception);
} ncm_v8handler;

static int our_execute(void *self, const cef_string_t *name, void *object,
                       size_t argc, void **argv, void **retval, cef_string_t *exception);

/* 注意 size 字段：CEF 用它做版本判定，所以填我们结构体的真实大小 */
static ncm_v8handler g_handler = {
    { sizeof(ncm_v8handler), h_add_ref, h_release, h_has_one, h_has_at_least },
    our_execute
};

/* ── 小工具 ─────────────────────────────────────────────────────────────── */

static void u8_to_u16(const char *s, uint16_t *out, int out_chars)
{
    if (out_chars <= 0) return;
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, (wchar_t *)out, out_chars);
    if (n <= 0) out[0] = 0;
}

static void u16_to_u8(const uint16_t *s, size_t len, char *out, int out_size)
{
    if (out_size <= 0) return;
    out[0] = 0;
    if (!s) return;
    if (len > 100000) len = 100000;
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)s, (int)len,
                                out, out_size - 1, NULL, NULL);
    if (n < 0) n = 0;
    out[n] = 0;
}

/** 造一个 JS 字符串返回给页面（CEF 接管所有权）。
 *
 * ★ 这里**故意不释放** w：
 *   实测（docs/07 §7.2.5）mode=5（调用返回但不读）存活，mode=1（JS 里对返回值
 *   做 .indexOf 读取）8 秒内整个应用退出 ⇒ cef_v8value_create_string 并不保证
 *   深拷贝传入的 cef_string_t，释放后 V8 读到悬空内存即崩。
 *   每次调用只泄漏几十字节，且调用次数有限（页面侧只在关键点打点），可以接受。 */
static void *make_js_string(const char *s)
{
    if (!g_create_string) return NULL;
    if (!s) s = "";
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (need <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, need);

    cef_string_t cs;
    cs.str    = (uint16_t *)w;
    cs.length = (size_t)(need - 1);
    cs.dtor   = NULL;

    return g_create_string(&cs);   /* w 不释放，交给 CEF 长期持有 */
}

/** 释放 CEF 给我们的 v8value（base 的第 2 个函数指针是 release）。 */
static void js_free(void *v)
{
    if (!v) return;
    void **base = (void **)v;
    void *rel = base[2];
    if (rel) {
        __try { ((int (*)(void *))rel)(v); }
        __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
}

/* ── get_string_value 槽位自检 ──────────────────────────────────────────── */
static int probe_string_getter(void)
{
    if (g_string_idx >= 0) return g_string_idx;
    if (!g_create_string) return -1;

    static const char *kTest = "NCM_PROBE_7a3f21";
    uint16_t w[64];
    u8_to_u16(kTest, w, 64);

    cef_string_t cs;
    cs.str = w;
    cs.length = 0;
    while (w[cs.length]) cs.length++;
    cs.dtor = NULL;

    void *v = g_create_string(&cs);
    if (!v) { ncm_log("原生桥 ❌ 造不出测试字符串"); return -1; }

    /* 候选顺序来自 CEF 91 的 cef_values.h 成员表（is_string≈8、get_string_value≈19），
     * 但**不信任**它：必须自检通过才采用。 */
    /* 实测教训：这版 libcef 的方法表比头文件**整体偏后**（页面 global 上
     * has_value_bykey 头文件是 21、实测是 26；frame 的 get_url 头文件 14、实测 19）。
     * 所以候选集合必须往上扩，不能只围着 19 打转。 */
    static const int cand[] = { 24, 25, 19, 20, 18, 21, 17, 22, 23, 16, 26, 27, 28, 29, 30,
                                15, 14, 31, 32, 33 };
    size_t want = cs.length;

    for (int i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
        int idx = cand[i];
        void *fn = ((void **)((uint8_t *)v + NCM_CEF_BASE_SIZE))[idx];
        if (!fn) continue;

        cef_string_t *r = NULL;
        __try { r = ((cef_string_t *(*)(void *))fn)(v); }
        __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
        if (!r || !r->str || r->length != want) continue;

        int same = 1;
        for (size_t k = 0; k < want; k++)
            if (r->str[k] != w[k]) { same = 0; break; }
        if (!same) continue;

        g_string_idx = idx;
        ncm_log("原生桥：get_string_value 自检通过 → 槽位 %d（返回 \"%s\"）", idx, kTest);
        break;
    }

    if (g_string_idx < 0) {
        ncm_log("原生桥 ❌ get_string_value 自检失败，桥只能收空参；各候选实测：");
        for (int i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++) {
            int idx = cand[i];
            void *fn = ((void **)((uint8_t *)v + NCM_CEF_BASE_SIZE))[idx];
            if (!fn) continue;
            cef_string_t *r = NULL;
            __try { r = ((cef_string_t *(*)(void *))fn)(v); }
            __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
            char got[80] = "(null)";
            if (r && r->str) {
                int n = (int)(r->length < 40 ? r->length : 40);
                WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)r->str, n, got, sizeof(got) - 1, NULL, NULL);
                got[n < (int)sizeof(got) ? n : (int)sizeof(got) - 1] = 0;
            }
            ncm_log("    idx%d → %s", idx, got);
        }
    }
    js_free(v);
    return g_string_idx;
}

static int arg_str(void **argv, size_t argc, size_t i, char *out, int out_size)
{
    if (out_size > 0) out[0] = 0;
    if (!argv || i >= argc || !argv[i] || g_string_idx < 0) return 0;

    void *v = argv[i];
    void *fn = ((void **)((uint8_t *)v + NCM_CEF_BASE_SIZE))[g_string_idx];
    if (!fn) return 0;

    cef_string_t *r = NULL;
    __try { r = ((cef_string_t *(*)(void *))fn)(v); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
    if (!r || !r->str) return 0;

    u16_to_u8(r->str, r->length, out, out_size);
    return 1;
}

/* ── 文件系统（桥的 fs.* 由原生实现，和 BetterNCM 的 ncm.fs 对齐） ─────── */

/** 惰性初始化：拿到 libcef 导出并自检 get_string_value 槽位。
 *
 * ★ 这个函数是**必需的**：默认路径（host.mode=0，不注册扩展）以前根本不会走到
 *   probe_string_getter()，于是 g_string_idx 一直是 -1，
 *   inject.c 里 eval 的返回值永远读不出来 —— 表现为"eval 自检一直失败"，
 *   而真正的 eval 其实早就成功了（docs/07 §7.2.8 的排查记录）。 */
int ncm_host_ensure(void)
{
    if (g_string_idx >= 0) return 1;

    HMODULE cef = GetModuleHandleA("libcef.dll");
    if (!cef) return 0;
    if (!g_create_string)
        g_create_string = (fn_create_string)GetProcAddress(cef, "cef_v8value_create_string");
    if (!g_create_string) return 0;

    return probe_string_getter();
}

/** 把任意 cef_v8value_t* 当字符串读出来（复用已自检通过的 get_string_value 槽位）。
 *  给 inject.c 的 eval 返回值用：这样"注入是否成功"就能拿到确凿证据。 */
/** 创建一个 JS 字符串值（cef_v8value_create_string）。给 inject.c 写页面全局用。 */
void *ncm_create_js_string(const char *utf8)
{
    if (!utf8) return NULL;
    if (!ncm_host_ensure()) return NULL;
    if (!g_create_string) return NULL;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 1) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wlen);

    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = (size_t)(wlen - 1);
    cs.dtor = NULL;

    void *r = NULL;
    __try { r = g_create_string(&cs); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
    free(w);
    return r;
}
int ncm_host_value_to_utf8(void *v, char *out, int out_size)
{
    if (out_size > 0) out[0] = 0;
    if (!v) return 0;
    if (!ncm_host_ensure()) return 0;
    if (g_string_idx < 0) return 0;

    void *fn = ((void **)((uint8_t *)v + NCM_CEF_BASE_SIZE))[g_string_idx];
    if (!fn) return 0;

    cef_string_t *r = NULL;
    __try { r = ((cef_string_t *(*)(void *))fn)(v); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
    if (!r || !r->str) return 0;

    u16_to_u8(r->str, r->length, out, out_size);
    return 1;
}

static char *read_file_utf8(const wchar_t *path)
{
    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return NULL;

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (64 << 20)) { fclose(f); return NULL; }

    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static int write_file_utf8(const wchar_t *path, const char *data, size_t len)
{
    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return 0;
    size_t put = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    return len ? (put == len) : 1;
}

static char *read_dir_json(const wchar_t *path)
{
    wchar_t pat[MAX_PATH + 8];
    _snwprintf_s(pat, MAX_PATH + 8, _TRUNCATE, L"%ls\\*", path);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;

    size_t cap = 4096, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) { FindClose(h); return NULL; }
    out[0] = 0;
    out[len++] = '[';

    int first = 1;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0)))
            continue;

        char nm[512];
        u16_to_u8((const uint16_t *)fd.cFileName, wcslen(fd.cFileName), nm, sizeof(nm));
        char ent[640];
        _snprintf_s(ent, sizeof(ent), _TRUNCATE, "%s{\"name\":\"%s\",\"dir\":%s}",
                    first ? "" : ",", nm,
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "true" : "false");

        size_t need = len + strlen(ent) + 4;
        if (need > cap) {
            cap = need * 2;
            char *nb = (char *)realloc(out, cap);
            if (!nb) break;
            out = nb;
        }
        memcpy(out + len, ent, strlen(ent));
        len += strlen(ent);
        out[len] = 0;
        first = 0;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    out[len] = 0;
    out[len + 1] = 0;   /* 末尾补 ']' */
    out[len] = ']';
    return out;
}

/* ── 分发 ──────────────────────────────────────────────────────────────── */

static char *dispatch(const char *op, const char *a, const char *b)
{
    char *out = NULL;

    if (!strcmp(op, "alive")) {
        out = (char *)malloc(256);
        if (out) _snprintf_s(out, 256, _TRUNCATE, "ncm-host pid=%lu calls=%ld",
                             GetCurrentProcessId(), g_calls);
        return out;
    }

    if (!strcmp(op, "log")) {
        trace_write(a);            /* ★ 同步落盘：崩溃前也能留下 */
        pend_push(a);              /* 再排队，由 worker 写进正式日志 */
        return _strdup("1");
    }

    /* 重扫插件目录并把整份 host JSON（manifest 原文 + 入口源码）交给 JS，
     * 页面侧的 ncm.reload() 就是靠它做热重载的 —— 原生侧不解析语义。 */
    if (!strcmp(op, "plugins") || !strcmp(op, "hostinfo")) {
        return ncm_host_json();
    }

    if (!strcmp(op, "fs.exists")) {
        wchar_t w[MAX_PATH * 2];
        u8_to_u16(a, (uint16_t *)w, MAX_PATH * 2);
        DWORD at = GetFileAttributesW(w);
        return _strdup((at != INVALID_FILE_ATTRIBUTES) ? "1" : "0");
    }

    if (!strcmp(op, "fs.readText")) {
        wchar_t w[MAX_PATH * 2];
        u8_to_u16(a, (uint16_t *)w, MAX_PATH * 2);
        return read_file_utf8(w);
    }

    if (!strcmp(op, "fs.writeText")) {
        wchar_t w[MAX_PATH * 2];
        u8_to_u16(a, (uint16_t *)w, MAX_PATH * 2);
        return _strdup(write_file_utf8(w, b, strlen(b)) ? "1" : "0");
    }

    if (!strcmp(op, "fs.readDir")) {
        wchar_t w[MAX_PATH * 2];
        u8_to_u16(a, (uint16_t *)w, MAX_PATH * 2);
        return read_dir_json(w);
    }

    {
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "原生桥：未实现的操作 \"%s\"", op);
        pend_push(msg);
    }
    return _strdup("");
}

static int our_execute(void *self, const cef_string_t *name, void *object,
                       size_t argc, void **argv, void **retval, cef_string_t *exception)
{
    (void)self; (void)object;
    if (exception) { exception->str = NULL; exception->length = 0; exception->dtor = NULL; }

    char nm[128] = "";
    if (name) u16_to_u8(name->str, name->length, nm, sizeof(nm));
    if (strcmp(nm, "__ncmHostCall") != 0) return 0;   /* 不是我们的 native，交回 CEF */

    InterlockedIncrement(&g_calls);

    char op[128] = "", a[8192] = "", b[8192] = "";
    arg_str(argv, argc, 0, op, sizeof(op));
    arg_str(argv, argc, 1, a, sizeof(a));
    arg_str(argv, argc, 2, b, sizeof(b));

    char *r = dispatch(op, a, b);
    if (retval) *retval = make_js_string(r ? r : "");
    free(r);

    InterlockedIncrement(&g_ok);
    return 1;   /* 已处理 */
}

/* ── 注册我们的扩展 ─────────────────────────────────────────────────────── */

/** JS 引导段：把 native 函数包成 __ncmHost.native(op,a,b)，并做一次自证 ping。
 *  这段会连同页面载荷一起注册成 CEF 扩展 → CEF 在**每个** v8 上下文里执行它。 */
static const char *kBootstrap =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = (typeof __ncmHost === 'object' && __ncmHost) ? __ncmHost : {};\n"
    /* ★ 必须在这一层就把返回值转成 JS 原生字符串：
     *   实测（docs/07 §7.2.6）直接对 native 返回值调用 .indexOf 会让整个应用 ~8s 退出，
     *   而先 String() 再操作完全正常 —— 返回的 v8value 属于"外部对象"，
     *   不该在 JS 里继续当字符串用。 */
    "__ncmHost.native = function (op, a, b) {\n"
    "  try {\n"
    "    var r = __ncmHostCall(String(op == null ? '' : op),\n"
    "                          String(a == null ? '' : a),\n"
    "                          String(b == null ? '' : b));\n"
    "    return (r == null) ? '' : String(r);\n"
    "  } catch (e) { return ''; }\n"
    "};\n"
    "(function () {\n"
    "  try {\n"
    /* ★ 绝不能读 location：实测（docs/07 §7.2.6）mode=9 只读一次 location.href，
     *   整个应用 ~8 秒内退出；同一位置改成不碰 location（mode=8）则完全正常。
     *   CEF 扩展会在多种上下文里执行，其中有的上下文没有合法 document/location，
     *   访问即原生崩溃（不是 JS 异常，try/catch 兜不住）。 */
    "    if (__ncmHost.native('alive').indexOf('ncm-host') >= 0)\n"
    "      __ncmHost.native('log', '\u9875\u9762 JS \u5df2\u8fd0\u884c\u5e76\u56de\u8c03\u5230 DLL');\n"
    "  } catch (e) {}\n"
    "})();\n";

/** 只声明 native 函数并包一层，**不调用**（mode=4 隔离用）。 */
static const char *kBootstrapDecl =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  return __ncmHostCall(String(op == null ? '' : op), String(a == null ? '' : a),\n"
    "                       String(b == null ? '' : b));\n"
    "};\n";

/** bootstrap + 只调 alive（不写日志，mode=5 隔离用）。 */
static const char *kBootstrapAliveOnly =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { return __ncmHostCall(String(op == null ? '' : op), String(a == null ? '' : a),\n"
    "                             String(b == null ? '' : b)); }\n"
    "  catch (e) { return ''; }\n"
    "};\n"
    "try { __ncmHost.native('alive'); } catch (e) {}\n";

/** mode=6：调一次并把返回值当字符串读（不调第二次）。 */
static const char *kBootstrapReadOnly =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { return __ncmHostCall(String(op == null ? '' : op), String(a == null ? '' : a),\n"
    "                             String(b == null ? '' : b)); }\n"
    "  catch (e) { return ''; }\n"
    "};\n"
    "try { var __r = __ncmHost.native('alive'); var __s = String(__r); } catch (e) {}\n";

/** mode=7：调两次，两次都不读返回值。 */
static const char *kBootstrapTwoCalls =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { return __ncmHostCall(String(op == null ? '' : op), String(a == null ? '' : a),\n"
    "                             String(b == null ? '' : b)); }\n"
    "  catch (e) { return ''; }\n"
    "};\n"
    "try { __ncmHost.native('alive'); __ncmHost.native('log', 'second-call'); } catch (e) {}\n";

/** mode=8：indexOf 判断 + log，不碰 location。 */
static const char *kBootstrapIndexOf =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { var r = __ncmHostCall(String(op == null ? '' : op), String(a == null ? '' : a),\n"
    "                              String(b == null ? '' : b));\n"
    "        return (r == null) ? '' : String(r); }\n"
    "  catch (e) { return ''; }\n"
    "};\n"
    "try { if (__ncmHost.native('alive').indexOf('ncm-host') >= 0) __ncmHost.native('log', 'indexOf-hit'); }\n"
    "catch (e) {}\n";

/** mode=9：只读 location.href 并记日志，不做 indexOf。 */
static const char *kBootstrapLocation =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { var r = __ncmHostCall(String(op == null ? '' : op), String(a == null ? '' : a),\n"
    "                              String(b == null ? '' : b));\n"
    "        return (r == null) ? '' : String(r); }\n"
    "  catch (e) { return ''; }\n"
    "};\n"
    "try { var __h = (typeof location !== 'undefined' && location.href) || 'no-location';\n"
    "      __ncmHost.native('log', 'loc=' + __h); } catch (e) {}\n";

/** mode=10：DOM 可用性探针（只读，不写 DOM）。
 *  载荷要挂徽标、读 localStorage，所以必须先证明这些在扩展上下文里安全。 */
static const char *kBootstrapDom =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { var r = __ncmHostCall(String(op == null ? '' : op), String(a == null ? '' : a),\n"
    "                              String(b == null ? '' : b));\n"
    "        return (r == null) ? '' : String(r); }\n"
    "  catch (e) { return ''; }\n"
    "};\n"
    "try {\n"
    "  var m = [];\n"
    "  m.push('win=' + (typeof window !== 'undefined' ? 'y' : 'n'));\n"
    "  m.push('doc=' + (typeof document !== 'undefined' ? 'y' : 'n'));\n"
    "  __ncmHost.native('log', 'dom1 ' + m.join(','));\n"
    "  m.push('body=' + ((typeof document !== 'undefined' && document.body) ? 'y' : 'n'));\n"
    "  __ncmHost.native('log', 'dom2 ' + m.join(','));\n"
    "  var de = document.documentElement;\n"
    "  __ncmHost.native('log', 'dom3 de=' + (de ? 'y' : 'n') + ' rs=' + document.readyState);\n"
    "  var ls = (typeof localStorage !== 'undefined' && localStorage) ? 'y' : 'n';\n"
    "  __ncmHost.native('log', 'dom4 localStorage=' + ls);\n"
    "} catch (e) { try { __ncmHost.native('log', 'dom ERR ' + e); } catch (e2) {} }\n";

/** mode=11：连续 6 次原生调用，完全不碰 DOM —— 隔离"调用次数/状态"因素。 */
static const char *kBootstrapManyCalls =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { var r = __ncmHostCall(String(op == null ? '' : op), String(a == null ? '' : a),\n"
    "                              String(b == null ? '' : b));\n"
    "        return (r == null) ? '' : String(r); }\n"
    "  catch (e) { return ''; }\n"
    "};\n"
    "try {\n"
    "  for (var i = 0; i < 6; i++) __ncmHost.native('log', 'call#' + i);\n"
    "} catch (e) {}\n";

/** mode=12：只探 typeof window（其余一律不碰）。 */
static const char *kBootstrapWin =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { var r = __ncmHostCall(String(op==null?'':op), String(a==null?'':a), String(b==null?'':b));\n"
    "        return (r == null) ? '' : String(r); } catch (e) { return ''; }\n"
    "};\n"
    "var __tw = 'threw';\n"
    "try { __tw = typeof window; } catch (e) { __tw = 'exc:' + e; }\n"
    "try { __ncmHost.native('log', 'typeof-window=' + __tw); } catch (e) {}\n";

/** mode=13：只探 typeof document（不碰 window）。 */
static const char *kBootstrapDoc =
    "var __ncmHostCall = (function(){ native function __ncmHostCall(); return __ncmHostCall; })();\n"
    "var __ncmHost = {}; \n"
    "__ncmHost.native = function (op, a, b) {\n"
    "  try { var r = __ncmHostCall(String(op==null?'':op), String(a==null?'':a), String(b==null?'':b));\n"
    "        return (r == null) ? '' : String(r); } catch (e) { return ''; }\n"
    "};\n"
    "var __td = 'threw';\n"
    "try { __td = typeof document; } catch (e) { __td = 'exc:' + e; }\n"
    "try { __ncmHost.native('log', 'typeof-document=' + __td); } catch (e) {}\n";

int ncm_host_register(void)
{
    if (g_registered) return 1;

    /* ── A/B 开关（排查用）：%LOCALAPPDATA%\NCMPlugin\host.mode
     *   0 = 完全不注册（对照组：验证"渲染进程猝死"到底是不是我们注册扩展造成的）
     *   1 = 只注册引导段（bootstrap，不含页面载荷）→ 只验证 native 桥本身通不通
     *   2 或文件不存在 = 完整（引导段 + payload/ncm-page.js） */
    /* DEFAULT 0 = do not register extension; page injection goes via
     * cef_v8context -> frame -> execute_java_script (inject.c). */
    int mode = 0;
    {
        const char *la = getenv("LOCALAPPDATA");
        if (la && *la) {
            char p[MAX_PATH * 2];
            _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\NCMPlugin\\host.mode", la);
            FILE *mf = NULL;
            wchar_t wp[MAX_PATH * 2];
            u8_to_u16(p, (uint16_t *)wp, MAX_PATH * 2);
            if (_wfopen_s(&mf, wp, L"rb") == 0 && mf) {
                char buf[16] = {0};
                size_t got = fread(buf, 1, sizeof(buf) - 1, mf);
                (void)got;
                fclose(mf);
                mode = atoi(buf);
            }
        }
    }
    if (mode == 0) {
        ncm_log("原生桥：host.mode=0 → 跳过注册（对照组）");
        return 0;
    }

    HMODULE cef = GetModuleHandleA("libcef.dll");
    if (!cef) { ncm_log("原生桥 ❌ libcef 不在内存"); return 0; }

    if (!g_register_ext)
        g_register_ext = (fn_register_extension)GetProcAddress(cef, "cef_register_extension");
    if (!g_create_string)
        g_create_string = (fn_create_string)GetProcAddress(cef, "cef_v8value_create_string");

    if (!g_register_ext || !g_create_string) {
        ncm_log("原生桥 ❌ 导出没拿到 register=%p create_string=%p",
                (void *)g_register_ext, (void *)g_create_string);
        return 0;
    }

    probe_string_getter();

    char *bundle = NULL;
    const char *tail = "";
    void *handler = (void *)&g_handler;

    if (mode == 3) {
        /* 隔离实验：只验证"调用 cef_register_extension 这个动作本身"是否致命
         * （代码平凡、handler 传 NULL、不含 native 声明） */
        tail = "var __ncmT = 1;";
        handler = NULL;
    } else if (mode == 4) {
        /* 加上我们自己的 cef_v8handler_t + native 声明，但**不调用** */
        tail = kBootstrapDecl;
    } else if (mode == 5) {
        /* bootstrap + 只调 alive（不调 log） */
        tail = kBootstrapAliveOnly;
    } else if (mode == 6) {
        /* 调一次并把返回值当字符串读（不再调第二次）→ 隔离"读返回值" */
        tail = kBootstrapReadOnly;
    } else if (mode == 7) {
        /* 调两次，但两次都不读返回值 → 隔离"第二次调用" */
        tail = kBootstrapTwoCalls;
    } else if (mode == 8) {
        /* indexOf 判断 + log，但不碰 location → 隔离"读返回值做判断" */
        tail = kBootstrapIndexOf;
    } else if (mode == 9) {
        /* 只读 location.href 并记日志，不做 indexOf → 隔离 location */
        tail = kBootstrapLocation;
    } else if (mode == 10) {
        /* DOM 可用性探针 */
        tail = kBootstrapDom;
    } else if (mode == 11) {
        /* 连续多次调用，不碰 DOM */
        tail = kBootstrapManyCalls;
    } else if (mode == 12) {
        tail = kBootstrapWin;
    } else if (mode == 13) {
        tail = kBootstrapDoc;
    } else if (mode == 14) {
        /* ★ 照宿主自己的写法复刻（var 声明 + 函数体内 native），并加一次**延迟**执行。
         *   宿主扩展实测就长这样：
         *     var channel; if (!channel) channel = {};
         *     (function(){ channel.call = function(...){ native function call(); return call(...); }; })();
         *   它从不显式读写 window/document —— 所以我们也先不碰，只验证 setTimeout 能不能用。 */
        tail =
            "var __ncmL;if(!__ncmL)__ncmL={};"
            "(function(){__ncmL.r=function(s){native function log();return log(s);};})();"
            "try{setTimeout(function(){__ncmL.r('timer-fired');},3000);__ncmL.r('scheduled');}"
            "catch(e){__ncmL.r('schedule-exc');}";
    } else if (mode == 15) {
        /* 同 14，但延迟回调里真的去碰 DOM（DOM 就绪后再碰，而不是上下文刚创建时） */
        tail =
            "var __ncmL;if(!__ncmL)__ncmL={};"
            "(function(){__ncmL.r=function(s){native function log();return log(s);};})();"
            "function __ncmGo(){"
            "try{var d=document;var b=d&&d.body;"
            "__ncmL.r('deferred doc='+(d?'y':'n')+' body='+(b?'y':'n')+' rs='+(d?d.readyState:'-'));"
            "if(b){var el=d.createElement('div');"
            "el.id='ncm-inject-badge';"
            "el.setAttribute('style','position:fixed;right:12px;bottom:12px;z-index:2147483647;"
            "padding:6px 10px;border-radius:6px;background:rgba(0,0,0,.72);color:#fff;"
            "font:12px/1.5 sans-serif;pointer-events:none');"
            "el.textContent='NCM Plugin Host';"
            "b.appendChild(el);__ncmL.r('badge-appended');}"
            "}catch(e){__ncmL.r('deferred-exc');}}"
            "try{setTimeout(__ncmGo,4000);__ncmL.r('scheduled-dom');}"
            "catch(e){__ncmL.r('schedule-exc');}";
    } else if (mode >= 2) {
        bundle = ncm_bundle_build();            /* __NCM_HOST__ + 页面载荷源码 */
        if (!bundle) { ncm_log("原生桥 ❌ 载荷组装失败"); return 0; }
        tail = bundle;
    } else {
        tail = kBootstrap;                      /* mode == 1：bootstrap + alive/log */
    }

    size_t need = strlen(kBootstrap) + strlen(tail) + 64;
    char *code = (char *)malloc(need);
    if (!code) { free(bundle); return 0; }
    if (mode >= 3 && mode != 3 + 1000 && mode != 2)   /* 3..15 全部只用 tail，不拼 kBootstrap */
        _snprintf_s(code, need, _TRUNCATE, "%s", tail);
    else
        _snprintf_s(code, need, _TRUNCATE, "%s%s", kBootstrap, tail);

    ncm_log("原生桥：host.mode=%d 扩展源码 %zu 字符，开头 220 字符 = %.220s",
            mode, strlen(code), code);

    uint16_t wname[64], *wcode = (uint16_t *)malloc((strlen(code) + 1) * 2);
    if (!wcode) { free(code); free(bundle); return 0; }
    u8_to_u16("ncm/host", wname, 64);
    u8_to_u16(code, wcode, (int)strlen(code) + 1);

    cef_string_t cs_name, cs_code;
    cs_name.str = wname; cs_name.length = 0; cs_name.dtor = NULL;
    while (wname[cs_name.length]) cs_name.length++;
    cs_code.str = wcode; cs_code.length = 0; cs_code.dtor = NULL;
    while (wcode[cs_code.length]) cs_code.length++;

    __try {
        g_register_ext(&cs_name, &cs_code, handler);
        g_registered = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ncm_log("原生桥 ❌ cef_register_extension 抛异常");
        g_registered = 0;
    }

    ncm_log("%s 已注册自己的 CEF 扩展 'ncm/host'（mode=%d，含 native 桥 __ncmHostCall）",
            g_registered ? "✅" : "❌", mode);
    /* ★ wcode / code 同样不释放：CEF 在**每个** v8 上下文里执行这段源码，
     *   时机晚于本函数返回，释放后必崩（与 make_js_string 同一个原因）。 */
    free(bundle);
    return g_registered;
}

int ncm_host_calls(void)        { return g_calls; }
int ncm_host_ok(void)           { return g_ok; }
int ncm_host_string_idx(void)   { return g_string_idx; }
int ncm_host_registered(void)   { return g_registered; }
