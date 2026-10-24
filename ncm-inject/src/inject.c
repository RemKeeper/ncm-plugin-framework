/**


 * inject.c — 注入层核心（v2：走 cef_execute_process / cef_initialize → cef_app_t）
 *
 * 链路：
 *
 *   [IAT 打补丁]  cloudmusic.dll 的 IAT 里
 *                 cef_execute_process / cef_initialize
 *                        ↓ 换成我们的实现，拿到 cef_app_t*
 *   把 app 的**最后一个成员**（get_render_process_handler）换成我们的 getter
 *                        ↓
 *   CEF 在子进程里向我们要 handler → 返回我们自建的 cef_render_process_handler_t
 *                        ↓ 某个槽位被调用，第二/三个参数是 frame
 *   读 frame 的 URL，是 orpheus:// 就 execute_java_script(bundle)
 *
 * ── 为什么选 get_render_process_handler 而不是 client_t 的 get_load_handler ──
 *   app_t 的它是**最后一个成员** ⇒ 偏移 = size - 8，只需运行时读一次 size。
 *   client_t 的 get_load_handler 夹在中间，必须知道成员总数才能定位；
 *   第一次实机跑（docs/07 §7.2.2）就是在这里翻车的：
 *   实测 size = 0xA8 而不是推算的 0x98，成员数对不上，偏移就无从确定。
 *
 * ── 为什么 20 个槽位全部铺成通用桩 ──
 *   render_process_handler 的成员顺序只有文档记忆可依。与其赌一个偏移，
 *   不如全铺上「转发 + 记日志」的桩：谁的第 2 个参数能通过 frame 校验，
 *   谁就是 on_context_created。猜错不会错注射，也不会崩。
 *
 * 所有可能猜错的调用都包在 SEH 里。
 */
#include "inject.h"
#include "mitm.h"

/* 资源 MITM 补丁开关：二分定位「窗口不显示」时先关掉它 */
#ifndef NCM_ENABLE_MITM_PATCH
#define NCM_ENABLE_MITM_PATCH 0
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * 编译开关：上下文内联探针（默认关）
 *
 * 打开后会在渲染进程里对 cef_v8context_t 做"槽位考古"（dump 方法指针、
 * 逐个试 enter/eval 组合）。这些调用**都带 SEH**，但实测过：
 * 一旦命中非函数指针的槽位就会把渲染进程整个带走，
 * 所以量产构建必须默认关闭，只在做取证实验时用
 * (-DNCM_ENABLE_CTX_PROBE=1 或把下面的 0 改成 1)。
 * 详见 docs/07 §7.5。
 * ══════════════════════════════════════════════════════════════════════════ */
/** 渲染进程页面注入探针（默认关）。
 *  打开后会做：ctx→get_frame、原生读写页面全局、16 个执行槽的 delete/值比对探测。
 *  这些调用都有 SEH，但会持续改写页面全局（__ncm_* 前缀），量产构建必须关闭。
 *  取证记录见 docs/07 §7.14。 */
#ifndef NCM_ENABLE_RENDERER_PROBE
#define NCM_ENABLE_RENDERER_PROBE 0
#endif

#ifndef NCM_ENABLE_CTX_PROBE
#define NCM_ENABLE_CTX_PROBE 0
#endif
#include "cef_capi.h"
#include "util.h"
#include "plugins.h"
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 执行通道冒烟测试（load_url javascript:）开关 */
#ifndef NCM_ENABLE_EXEC_SMOKE2
#define NCM_ENABLE_EXEC_SMOKE2 1
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * 状态
 * ══════════════════════════════════════════════════════════════════════════ */

static int g_installed = 0;
static int g_hook_count = 0;
static int g_slot_calls = 0;
static int g_fwd_calls = 0;
static int g_injections = 0;

static void *g_slot_exec = NULL;      /* IAT 槽位 */
static void *g_slot_init = NULL;
static void *g_orig_exec = NULL;      /* libcef 真身 */

/* ══════════════════════════════════════════════════════════════════════════
 * 决定性实验：CEF 到底看不看 cef_app_t 这张表？
 *
 * 做法：把 app 的 6 个成员**全部**换成「记一笔 + 原样转发」的桩。
 * 只要 CEF 在任何一个进程里读过其中任何一个成员，trace 里就会留下
 *   app 成员 idxN 被调用
 * 反之如果 6 个全是 0 —— 那就说明这版 CEF 在子进程里根本不再问 app 要回调，
 * 「换 get_render_process_handler 做页面注入」这条路是**版本行为已变**，
 * 不是我们写错了。这一条直接决定下一阶段该往哪走（docs/07 §7.10）。
 * ══════════════════════════════════════════════════════════════════════════ */
#ifndef NCM_ENABLE_APP_PROBE
#define NCM_ENABLE_APP_PROBE 0
#endif

static void *g_app_orig[6];
static volatile LONG g_app_hits[6];

static void *app_slot(int idx, void *self, void *a, void *b)
{
    LONG n = InterlockedIncrement(&g_app_hits[idx]);
    if (n <= 2)
        ncm_trace("app 成员 idx%d 被调用（第 %ld 次）self=%p a=%p b=%p", idx, n, self, a, b);
    void *fn = g_app_orig[idx];
    if (!fn) return NULL;
    void *r = NULL;
    __try { r = ((void *(*)(void *, void *, void *))fn)(self, a, b); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = NULL; }
    return r;
}

static void *app_slot0(void *s, void *a, void *b) { return app_slot(0, s, a, b); }
static void *app_slot1(void *s, void *a, void *b) { return app_slot(1, s, a, b); }
static void *app_slot2(void *s, void *a, void *b) { return app_slot(2, s, a, b); }
static void *app_slot3(void *s, void *a, void *b) { return app_slot(3, s, a, b); }
static void *app_slot4(void *s, void *a, void *b) { return app_slot(4, s, a, b); }
static void *app_slot5(void *s, void *a, void *b) { return app_slot(5, s, a, b); }

static void *const g_app_stubs[6] = {
    (void *)app_slot0, (void *)app_slot1, (void *)app_slot2,
    (void *)app_slot3, (void *)app_slot4, (void *)app_slot5
};
static void *g_orig_init = NULL;

static void *g_app = NULL;
static size_t g_app_size = 0;
static uintptr_t g_rph_off = 0;       /* app 里 get_render_process_handler 的偏移 */
static void *g_orig_get_rph = NULL;
static void *g_orig_rph = NULL;       /* NCM 自己的 render_process_handler（用于转发） */

static int g_ctx_slot = -1;           /* 实测出来的 on_context_created 槽位 */
static int g_app_patched = 0;

static uintptr_t g_frame_base_off = 0;
static uintptr_t g_frame_url_off = 0;
static uintptr_t g_frame_execjs_off = 0;

#define MAX_INJ_FRAMES 32
static uintptr_t g_inj_frames[MAX_INJ_FRAMES];
static int       g_inj_frame_n = 0;

/* ══════════════════════════════════════════════════════════════════════════
 * 布局工具
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * 扫出「回调指针数组」的起始偏移。
 *
 * ★ 从 0x28 起步，不是 0x18。
 *   第一次实机跑就栽在这：cef_base_ref_counted_t 的最后两个成员
 *   (has_one_ref @0x18 / has_at_least_one_ref @0x20) 本身也是代码指针，
 *   从 0x18 开始数"连续 N 个代码指针"会被它们带偏，锚点整体前移 0x10。
 */
static int scan_array_base(const void *obj, int need, uintptr_t *out_off)
{
    const uintptr_t *p = (const uintptr_t *)obj;
    for (uintptr_t off = NCM_CEF_BASE_SIZE; off <= 0x48; off += 8) {
        int ok = 1;
        for (int i = 0; i < need; i++) {
            if (!ncm_ptr_is_code(p[off / 8 + i])) { ok = 0; break; }
        }
        if (ok) { *out_off = off; return 1; }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 从 cef_v8context_t 反推 cef_frame_t
 *
 * 为什么需要：实测渲染进程里 CEF **从不**向 app 要 get_render_process_handler
 * （0 次），但 cef_v8context_get_current_context 被调 1039 次。
 * 于是改走"上下文 → frame → execute_java_script"这条路，
 * 它等价于 BetterNCM 的做法，且跑在**页面上下文**里（DOM 完整）。
 * ══════════════════════════════════════════════════════════════════════════ */

#define NCM_FRAME_EXPECTED_SIZE 0x58u
static uintptr_t g_ctx_frame_off = 0;

/* 这几个 helper 定义在本块之后，先声明 */
static int   try_call_get_url(void *frame, uintptr_t fn, char *out_url, int out_size);
static int   looks_like_url(const char *s);
static int   resolve_frame_api(void *frame);
static void  try_inject(void *frame, const char *url);

/** 候选偏移 + 行为校验。
 *
 * 设计转变（实测教训）：原先先用「base.size 必须是 0x58」预筛，
 * 结果 5 个候选偏移**全部**被这一步挡掉（日志里一条 frame: 都没进）。
 * 说明这版 CEF 的 cef_frame_t 大小不是 0x58，或 get_frame 不在猜的偏移上。
 * 于是改成**只信行为**：把 ctx 的每个 8 字节字段都当成可能的 frame 指针，
 * 用「能不能通过 get_url 返回合法 URL」判定 —— 这是唯一不会骗人的证据。
 * 每个候选调用都套 SEH，误判只会被吞掉。 */
static void *resolve_ctx_frame(void *ctx)
{
    if (!ctx) return NULL;

    if (g_ctx_frame_off) {
        void *f = NULL;
        __try { f = *(void **)((uint8_t *)ctx + g_ctx_frame_off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
        return f;
    }

    /* CEF 91 的 cef_frame_t 成员序：
     *   0 is_valid  1 undo  2 redo  3 cut  4 copy  5 paste  6 del  7 select_all
     *   8 view_source  9 get_source  10 get_text  11 load_request  12 load_url
     *   13 execute_java_script  14 get_url  15 get_browser ...
     * get_url 紧跟 execute_java_script，所以拿到 get_url 槽位就能推出 execjs。
     * 但**不信任**顺序：11..16 全试，谁能返回 URL 谁就是 get_url。 */
    for (uintptr_t off = 0x28; off <= 0x110; off += 8) {
        void *p = NULL;
        __try { p = *(void **)((uint8_t *)ctx + off); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!p) continue;

        /* 便宜的合理性筛：对象头 size 字段通常是个小整数 */
        size_t sz = 0;
        __try { sz = *(size_t *)p; }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (sz < 0x40 || sz > 0x300) continue;

        for (int idx = 11; idx <= 16; idx++) {
            uintptr_t fn = 0;
            __try { fn = *(uintptr_t *)((uint8_t *)p + NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (!ncm_ptr_is_code(fn)) continue;

            char url[1024];
            if (try_call_get_url(p, fn, url, sizeof(url)) != 1) continue;
            if (!looks_like_url(url)) continue;

            g_ctx_frame_off    = off;
            g_frame_base_off   = NCM_CEF_BASE_SIZE;
            g_frame_url_off    = NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8;
            g_frame_execjs_off = NCM_CEF_BASE_SIZE + (uintptr_t)(idx - 1) * 8;
            ncm_trace("ctx ✅ 命中：ctx+0x%llX 是 frame，get_url=idx%d，size=0x%zX，URL=%s",
                      (unsigned long long)off, idx, sz, url);
            return p;
        }
    }

    ncm_trace("ctx ❌ 0x28..0x110 全字段试过，没有能返回 URL 的 frame 指针");
    return NULL;
}

/** 从上下文拿 frame：能拿到且有 URL 就注入（每个 frame 只做一次）。 */
static void ctx_try_inject(void *ctx)
{
    void *f = resolve_ctx_frame(ctx);
    if (!f) return;

    char url[1024];
    uintptr_t fn = *(uintptr_t *)((uint8_t *)f + g_frame_url_off);
    if (try_call_get_url(f, fn, url, sizeof(url)) != 1) return;
    if (!looks_like_url(url)) return;

    if (strncmp(url, "orpheus://", 10) == 0)
        try_inject(f, url);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 直接在上下文里 eval —— 比 frame 更短的路径
 *
 * 为什么再换一条：实测（docs/07 §7.2.7）从 ctx 字段暴力枚举 frame 全部失败
 * （0x28..0x110 每个字段都试过，没有一个能返回合法 URL）。
 * 但 ctx 自己就带 eval，能在**当前上下文**里跑代码，不需要 frame。
 * 槽位同样不猜：先用 `(function(){return "NCM_EVAL_OK"})()` 自检，
 * 谁的槽位能原样返回这个字符串谁就是 eval —— 与 get_string_value 同一套办法。
 * ══════════════════════════════════════════════════════════════════════════ */

static int g_ctx_eval_idx = -1;      /* eval 的槽位（自检通过后固定） */
static int g_ctx_enter_idx = 4;      /* enter / exit（CEF 头文件顺序） */
static int g_ctx_exit_idx  = 5;

static int ctx_call_int(void *ctx, int idx)
{
    void *fn = ((void **)((uint8_t *)ctx + NCM_CEF_BASE_SIZE))[idx];
    if (!fn) return 0;
    int r = 0;
    __try { r = ((int (*)(void *))fn)(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}

/** cef_string_t → UTF-8（仅诊断用） */
static void cefstr_dbg(const cef_string_t *s, char *out, int n)
{
    if (n > 0) out[0] = 0;
    if (!s || !s->str || !s->length) return;
    int need = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)s->str, (int)s->length,
                                   NULL, 0, NULL, NULL);
    if (need <= 0) return;
    if (need >= n) need = n - 1;
    WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)s->str, (int)s->length,
                        out, need, NULL, NULL);
    out[need] = 0;
}

static int ctx_eval_raw(void *ctx, int eval_idx, const char *code,
                        char *out, int out_size)
{
    if (out_size > 0) out[0] = 0;
    void *fn = ((void **)((uint8_t *)ctx + NCM_CEF_BASE_SIZE))[eval_idx];
    if (!fn) return -1;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, code, -1, NULL, 0);
    if (wlen <= 1) return -1;
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return -1;
    MultiByteToWideChar(CP_UTF8, 0, code, -1, w, wlen);

    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = (size_t)(wlen - 1);
    cs.dtor = NULL;

    void *ret = NULL;
    cef_string_t exc;
    exc.str = NULL; exc.length = 0; exc.dtor = NULL;

    /* script_url 不能传 NULL：CEF 内部会解引用它（实测传 NULL 直接 AV）。 */
    static const uint16_t kUrl[] = { 0x6E,0x63,0x6D,0x3A,0x2F,0x2F,0x70,0x61,0x67,0x65,0 };
    cef_string_t us; us.str = (uint16_t *)kUrl; us.length = 10; us.dtor = NULL;

    int ok = 0;
    __try {
        /* int eval(self, code, script_url, retval, exception) */
        ok = ((int (*)(void *, const cef_string_t *, const cef_string_t *,
                       void **, cef_string_t *))fn)(ctx, &cs, &us, &ret, &exc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = -2;
    }
    free(w);

    /* ★ 把原始返回值也打出来：ok=1 但 out 为空 ⇒ 槽位对、只是取值失败；
     *   ok=0 且 exc 有内容 ⇒ 槽位对、脚本本身有问题；
     *   ok=-2 ⇒ 槽位错（调用抛异常）。三种情况处理方式完全不同。 */
    {
        char ex[96];
        cefstr_dbg(&exc, ex, sizeof(ex));
        ncm_trace("eval idx%d: ok=%d ret=%p out='%s' exc='%s'",
                  eval_idx, ok, ret, out, ex);
    }

    if (ok <= 0) return -1;
    if (ret) ncm_host_value_to_utf8(ret, out, out_size);
    return 1;
}

/** ★ 必须在 enter()/exit() 之间 eval：V8 要求有"当前上下文"，
 *  否则 eval 直接失败甚至崩。这是 CEF C++ 包装层内部自己做的事
 *  （BetterNCM 用 C++ API 所以不用管），走纯 C 接口就得自己来。 */
static int ctx_eval_cand(void *ctx, int eval_idx, const char *code,
                         char *out, int out_size)
{
    int entered = ctx_call_int(ctx, g_ctx_enter_idx);
    int r = ctx_eval_raw(ctx, eval_idx, code, out, out_size);
    if (entered) ctx_call_int(ctx, g_ctx_exit_idx);
    return r;
}

/** 自检并固定 eval 槽位；返回 1 表示可用。 */
static int ctx_resolve_eval(void *ctx)
{
    if (g_ctx_eval_idx >= 0) return 1;
    if (!ctx) return 0;

    /* 最后一组没试过的组合：enter(1/5/6) + eval=idx7 + **合法** script_url。
     * 之前试过一次，但那时 url 是 NULL，eval 会静默失败，结论不作数。 */
    static const int enters[] = { 1, 5, 6 };
    char out[64];
    for (int i = 0; i < 3; i++) {
        int e = enters[i];
        int en = ctx_call_int(ctx, e);
        int r = ctx_eval_raw(ctx, 7, "(1+1)+''", out, sizeof(out));
        ncm_trace("enter=idx%d(→%d) + eval7(合法url) → r=%d out='%s'", e, en, r, out);
        if (r == 1 && strcmp(out, "2") == 0) {
            g_ctx_enter_idx = e;
            g_ctx_eval_idx = 7;
            ncm_trace("ctx ✅✅ eval 通了！enter=idx%d eval=idx7，1+1 → '%s'", e, out);
            return 1;
        }
        ctx_call_int(ctx, 1); ctx_call_int(ctx, 5); ctx_call_int(ctx, 6);   /* 复位 */
    }
    ncm_trace("ctx ❌ enter(1/5/6)+eval7+合法url 依然不通");
    return 0;
}





/* 页码侦测：只问"这里有没有 DOM"，用返回值自证，不靠猜。 */
static const char *kDomProbe =
    "(function(){try{"
    "var d=(typeof document);"
    "var h=(typeof location);"
    "if(d==='undefined')return 'no-document-loc='+h;"
    "return 'doc='+d+',body='+((document.body)?'y':'n')+',rs='+document.readyState+',loc='+h;"
    "}catch(e){return 'exc:'+e}})()";

/** 在上下文里 eval：先探 DOM，DOM 可用才注入完整载荷。 */
static void ctx_eval_inject(void *ctx)
{
    if (!ctx || g_injections) return;
    if (!ctx_resolve_eval(ctx)) return;

    char probe[512];
    if (ctx_eval_raw(ctx, g_ctx_eval_idx, kDomProbe, probe, sizeof(probe)) != 1) {
        ncm_trace("ctx eval 探针没返回结果");
        return;
    }
    ncm_trace("ctx 上下文环境 = %s", probe);

    /* 只有确认拿到真 DOM 才注入载荷；否则宁可不动，也不去赌。 */
    int has_body = (strstr(probe, "body=y") != NULL);
    if (!has_body) {
        ncm_trace("ctx 该上下文没有可用的 document.body → 跳过（%s）", probe);
        return;
    }

    char *bundle = ncm_bundle_build();
    if (!bundle) { ncm_trace("ctx bundle 组装失败"); return; }

    char out[256];
    int r = ctx_eval_raw(ctx, g_ctx_eval_idx, bundle, out, sizeof(out));
    if (r == 1) {
        g_injections++;
        ncm_trace("✅ 页面载荷已通过 eval 注入（%zu 字节），返回=%s", strlen(bundle), out);
    } else {
        ncm_trace("❌ 载荷 eval 失败 r=%d", r);
    }
    free(bundle);
}

/* ══════════════════════════════════════════════════════════════════════════
 * frame：URL 读取 + JS 执行（都带 SEH + 行为校验）
 * ══════════════════════════════════════════════════════════════════════════ */

static int try_call_get_url(void *frame, uintptr_t fn, char *out_url, int out_size)
{
    __try {
        cef_string_userfree_t s = ((cef_string_userfree_t (*)(void *))fn)(frame);
        if (!s) return 0;
        int n = ncm_cefstr_to_utf8(s, out_url, out_size);
        /* 不 free：CEF 的 userfree 串要走它自己的分配器释放，
         * 宁可漏这几个字节也不要 free 错（docs/07 §7.2.4） */
        return n > 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static int looks_like_url(const char *s)
{
    if (!s || !s[0]) return 0;
    static const char *pfx[] = { "http://", "https://", "orpheus://", "devtools://",
                                 "file://", "chrome://", "about:" };
    for (int i = 0; i < 7; i++)
        if (strncmp(s, pfx[i], strlen(pfx[i])) == 0) return 1;
    return 0;
}

/** 解析 frame 的 get_url / execute_java_script 偏移（带行为校验，只做一次）。 */
static int resolve_frame_api(void *frame)

{
    if (g_frame_execjs_off && g_frame_url_off) return 1;
    if (!frame) return 0;

    uintptr_t base = 0;
    if (!scan_array_base(frame, 14, &base)) {
        ncm_log("frame: 扫不到回调数组起始偏移（obj=%p 可能不是 cef_frame_t）", frame);
        return 0;
    }
    ncm_log("frame: 回调数组基址偏移 = 0x%llX（期望 0x28）", (unsigned long long)base);

    /* 候选顺序：先试 idx13（最可能是 get_url），再试 idx14（get_browser，
     * 调用无副作用），最后才试 idx12（execute_java_script，签名长、误调代价最大）。 */
    const int cands[] = { 13, 14, 12 };
    for (int ci = 0; ci < 3; ci++) {
        uintptr_t off = base + (uintptr_t)cands[ci] * 8;
        uintptr_t fn  = *(uintptr_t *)((uint8_t *)frame + off);
        if (!ncm_ptr_is_code(fn)) continue;

        char url[1024];
        int r = try_call_get_url(frame, fn, url, sizeof(url));
        if (r < 0) { ncm_log("frame: idx%d(0x%llX) 调用抛异常 → 跳过", cands[ci], (unsigned long long)off); continue; }
        if (r == 0) { ncm_log("frame: idx%d(0x%llX) 返回值不像 URL → 跳过", cands[ci], (unsigned long long)off); continue; }

        ncm_log("frame ✅ idx%d = get_url：off=0x%llX  实测 URL = %s",
                cands[ci], (unsigned long long)off, url);
        g_frame_url_off = off;

        uintptr_t eoff = base + (uintptr_t)(cands[ci] - 1) * 8;
        uintptr_t efn  = *(uintptr_t *)((uint8_t *)frame + eoff);
        if (ncm_ptr_is_code(efn)) {
            g_frame_execjs_off = eoff;
            char d[192];
            ncm_describe_addr(efn, d, sizeof(d));
            ncm_log("frame ✅ execute_java_script = off 0x%llX → %s",
                    (unsigned long long)eoff, d);
        }
        g_frame_base_off = base;
        break;
    }

    if (!g_frame_execjs_off) { ncm_log("frame ❌ 没能确定 execute_java_script 偏移"); return 0; }
    return 1;
}

static int frame_run_js(void *frame, const char *code, int code_len)
{
    if (!frame || !g_frame_execjs_off || !code) return 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, code, code_len, NULL, 0);
    if (wlen <= 0) return 0;
    wchar_t *w = (wchar_t *)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
    if (!w) return 0;
    MultiByteToWideChar(CP_UTF8, 0, code, code_len, w, wlen);
    w[wlen] = 0;

    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = (size_t)wlen;
    cs.dtor = NULL;

    int ok = 0;
    uintptr_t fn = *(uintptr_t *)((uint8_t *)frame + g_frame_execjs_off);
    __try {
        ((ncm_frame_execute_js)fn)(frame, &cs, NULL, 0);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
    }
    free(w);
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 注入动作
 * ══════════════════════════════════════════════════════════════════════════ */

static int frame_already_injected(uintptr_t f)
{
    for (int i = 0; i < g_inj_frame_n; i++)
        if (g_inj_frames[i] == f) return 1;
    return 0;
}

static void try_inject(void *frame, const char *url)
{
    if (frame_already_injected((uintptr_t)frame)) return;

    char *bundle = ncm_bundle_build();
    if (!bundle) { ncm_log("bundle 组装失败"); return; }

    size_t blen = strlen(bundle);
    ncm_log("开始注入 URL=%s  frame=%p  bundle=%zuB  插件=%d 个",
            url, frame, blen, ncm_plugins_count());

    if (frame_run_js(frame, bundle, (int)blen)) {
        g_injections++;
        if (g_inj_frame_n < MAX_INJ_FRAMES) g_inj_frames[g_inj_frame_n++] = (uintptr_t)frame;
        ncm_log("注入 ✅ 已投递（第 %d 次）", g_injections);
    } else {
        ncm_log("注入 ❌ execute_java_script 调用失败");
    }
    free(bundle);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 我们自建的 cef_render_process_handler_t
 * ══════════════════════════════════════════════════════════════════════════ */

static long g_refcount = 1;
static void rph_add_ref(void *s)   { (void)s; InterlockedIncrement(&g_refcount); }
static int  rph_release(void *s)   { (void)s; return InterlockedDecrement(&g_refcount) == 0; }
static int  rph_has_one(void *s)   { (void)s; return g_refcount == 1; }
static int  rph_has_at_least(void *s) { (void)s; return g_refcount >= 1; }

static ncm_cef_rph g_our_rph;
static int        g_slot_seen[NCM_RPH_SLOTS];
static int        g_logged_nonorpheus = 0;
static volatile LONG g_get_rph_calls = 0;   /* CEF 到底有没有来问过我们要 handler */

/**
 * 通用槽位桩：转发到原实现 + 记录 + （可能是 on_context_created 时）注入。
 *
 * 参数最多 6 个：CEF 最长的回调是 on_uncaught_exception(self,browser,frame,
 * context,exception,stack_trace)，正好 6 个。多出来的形参在被调方会被忽略，
 * 所以用同一个签名兜所有槽位是安全的。
 */
static void *generic_slot(int idx, void *self, void *a, void *b, void *c, void *d, void *e)
{
    (void)self;
    g_slot_calls++;

    if (idx >= 0 && idx < (int)NCM_RPH_SLOTS && !g_slot_seen[idx]) {
        g_slot_seen[idx] = 1;
        ncm_log("rph 槽位 %d 首次被调用：arg1=%p arg2=%p arg3=%p", idx, a, b, c);
    }

    /* ── 是不是 on_context_created？判据：第 2 个参数能当 frame 用 ──
     * 用行为校验而不是靠成员顺序：真 frame 的 get_url 会返回一个像 URL 的串。 */
    if (idx >= 3 && idx <= 9 && b && !g_injections) {
        if (resolve_frame_api(b) && g_frame_url_off) {
            char url[1024] = {0};
            uintptr_t fn = *(uintptr_t *)((uint8_t *)b + g_frame_url_off);
            if (ncm_ptr_is_code(fn) && try_call_get_url(b, fn, url, sizeof(url)) == 1
                && looks_like_url(url)) {
                if (g_ctx_slot < 0) {
                    g_ctx_slot = idx;
                    ncm_log("★ 实测认定：槽位 %d 是 on_context_created（arg2 过了 frame 校验，URL=%s）",
                            idx, url);
                }
                if (strncmp(url, "orpheus://", 10) == 0) {
                    ncm_log("命中目标页面：%s", url);
                    try_inject(b, url);
                } else if (!g_logged_nonorpheus) {
                    g_logged_nonorpheus = 1;
                    ncm_log("跳过非 orpheus 上下文（只记第一次）：%s", url);
                }
            }
        }
    }

    /* ── 转发给 NCM 自己的 handler，保证原有功能不受影响 ──
     * 注意 self 必须换成原对象：CEF 是用**我们的**指针调进来的，
     * 但原实现要收到它自己的指针。 */
    if (g_orig_rph && idx >= 0 && idx < (int)NCM_RPH_SLOTS) {
        uintptr_t fn = *(uintptr_t *)((uint8_t *)g_orig_rph + NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8);
        if (ncm_ptr_is_code(fn)) {
            __try {
                void *r = ((ncm_rph_slot_fn)fn)(g_orig_rph, a, b, c, d, e);
                g_fwd_calls++;
                return r;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ncm_log("槽位 %d 转发生效时抛异常（已吞掉）", idx);
            }
        }
    }
    return NULL;
}

/* 20 个各不相同的桩 —— 必须不同，否则分不清 CEF 调的是哪个槽位 */
#define DEF_STUB(N) \
    static void *stub_##N(void *s, void *a, void *b, void *c, void *d, void *e) \
    { return generic_slot(N, s, a, b, c, d, e); }
DEF_STUB(0)  DEF_STUB(1)  DEF_STUB(2)  DEF_STUB(3)  DEF_STUB(4)
DEF_STUB(5)  DEF_STUB(6)  DEF_STUB(7)  DEF_STUB(8)  DEF_STUB(9)
DEF_STUB(10) DEF_STUB(11) DEF_STUB(12) DEF_STUB(13) DEF_STUB(14)
DEF_STUB(15) DEF_STUB(16) DEF_STUB(17) DEF_STUB(18) DEF_STUB(19)

static ncm_rph_slot_fn g_stubs[NCM_RPH_SLOTS] = {
    stub_0,  stub_1,  stub_2,  stub_3,  stub_4,
    stub_5,  stub_6,  stub_7,  stub_8,  stub_9,
    stub_10, stub_11, stub_12, stub_13, stub_14,
    stub_15, stub_16, stub_17, stub_18, stub_19,
};

static void build_our_rph(void)
{
    memset(&g_our_rph, 0, sizeof(g_our_rph));
    g_our_rph.base.size = (size_t)sizeof(g_our_rph);
    g_our_rph.base.add_ref = (void (*)(ncm_cef_base_ref_counted *))rph_add_ref;
    g_our_rph.base.release = (int (*)(ncm_cef_base_ref_counted *))rph_release;
    g_our_rph.base.has_one_ref = (int (*)(ncm_cef_base_ref_counted *))rph_has_one;
    g_our_rph.base.has_at_least_one_ref = (int (*)(ncm_cef_base_ref_counted *))rph_has_at_least;
    for (unsigned i = 0; i < NCM_RPH_SLOTS; i++) g_our_rph.slot[i] = g_stubs[i];
}

static void *our_get_rph(void *app)
{
    InterlockedIncrement(&g_get_rph_calls);
    if (!g_orig_rph && g_orig_get_rph) {
        __try { g_orig_rph = ((void *(*)(void *))g_orig_get_rph)(app); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_orig_rph = NULL; }

        if (g_orig_rph) {
            size_t osz = *(size_t *)g_orig_rph;
            char d[192];
            ncm_describe_addr((uintptr_t)g_orig_rph, d, sizeof(d));
            ncm_log("拿到 NCM 原始 render_process_handler = %s  base.size=0x%zX（CEF 91 期望 0x%X）",
                    d, osz, NCM_RPH_EXPECTED_SIZE);
            if (osz != NCM_RPH_EXPECTED_SIZE)
                ncm_log("⚠️ 原 handler 大小与预期不符 —— 成员顺序假设可能有偏差，"
                        "但槽位是行为校验出来的，不影响正确性");
        } else {
            ncm_log("NCM 没有提供 render_process_handler（不转发）");
        }
    }
    static int logged = 0;
    if (!logged) { logged = 1; ncm_log("CEF 向我们要 get_render_process_handler → 返回自建 handler"); }
    return &g_our_rph;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 把 app 的最后一个成员换成我们的 getter
 * ══════════════════════════════════════════════════════════════════════════ */

static int attach_app(void *app)
{
    if (!app || g_app_patched) return 0;

    size_t size = *(size_t *)app;

    /* ── 结构体大小先做合理性检查 ──
     * off = size - 8 是拿 size 算出来的，size 要是垃圾值，off 就是垃圾偏移，
     * 往那儿写会直接踩坏内存。已知 CEF 91 是 0x58，留足余量只挡明显离谱的值。 */
    if (size < 0x28 || size > 0x400) {
        ncm_log("❌ app=%p 的 base.size=0x%zX 不合理，拒绝对它动手", app, size);
        return 0;
    }

    uintptr_t off = (uintptr_t)size - 8;      /* get_render_process_handler 是最后一个成员 */
    void **slot = (void **)((uint8_t *)app + off);
    void *cur = *slot;

    /* ★ 不要在这里建模块表（psapi）—— 这个函数跑在 CEF 回调线程上，
     *   而此刻日志（以及它的 CRITICAL_SECTION）可能还没初始化。
     *   历史上就是这一行把主线程锁死、子进程一个都生不出来（docs/07 §7.2.3）。
     *   ncm_ptr_is_code 现在自己走无锁 PEB 走链，不需要表。 */

    char d[192];
    ncm_describe_addr((uintptr_t)cur, d, sizeof(d));
    ncm_log("app=%p  base.size=0x%zX（CEF 91 实测 0x58；我们当初推算 0x%X）→ 末成员偏移 0x%llX",
            app, size, NCM_APP_EXPECTED_SIZE, (unsigned long long)off);
    if (size != NCM_APP_EXPECTED_SIZE)
        ncm_log("   成员数比推算的多 —— 但 size-8 仍然是最后一个成员，推导不受影响");
    ncm_log("app+0x%llX 当前值 = %s", (unsigned long long)off, d);
    /* 把所有成员都摊开：CEF 91 头文件顺序是
     * base(0x28) on_before_command_line_processing / on_register_custom_schemes /
     * get_resource_bundle_handler / get_browser_process_handler / get_render_process_handler。
     * 哪个槽是 NULL、哪个是代码指针，一眼就能看出真实布局。 */
    for (uintptr_t fo = 0x28; fo <= 0x58; fo += 8) {
        void *fv = NULL;
        __try { fv = *(void **)((uint8_t *)app + fo); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        char fd[160];
        ncm_describe_addr((uintptr_t)fv, fd, sizeof(fd));
        ncm_trace("app+0x%02llX = %s", (unsigned long long)fo, fd);
    }
    if (!ncm_ptr_is_code((uintptr_t)cur)) {
        ncm_log("❌ app+0x%llX 不是代码指针，放弃替换", (unsigned long long)off);
        return 0;
    }

    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old_prot)) {
        ncm_log("❌ VirtualProtect 失败 err=%lu", GetLastError());
        return 0;
    }
    g_orig_get_rph = cur;
    *slot = (void *)our_get_rph;
    VirtualProtect(slot, sizeof(void *), old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));

    g_app = app;
    g_app_size = size;
    g_rph_off = off;
    g_app_patched = 1;
    ncm_log("app ✅ get_render_process_handler 已替换（app+0x%llX）", (unsigned long long)off);

    /* 决定性实验：把 0x28..0x50 六个成员全部换成转发桩（默认关闭） */
    if (NCM_ENABLE_APP_PROBE && size >= 0x58) {
        for (int i = 0; i < 6; i++) {
            uintptr_t fo = 0x28 + (uintptr_t)i * 8;
            void **fs = (void **)((uint8_t *)app + fo);
            g_app_orig[i] = *fs;
            DWORD op = 0;
            if (VirtualProtect(fs, sizeof(void *), PAGE_EXECUTE_READWRITE, &op)) {
                *fs = g_app_stubs[i];
                VirtualProtect(fs, sizeof(void *), op, &op);
            }
        }
        ncm_trace("app 探针已装：0x28..0x50 六个成员全部换成转发桩");
    }
    return 1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * IAT 补丁
 * ══════════════════════════════════════════════════════════════════════════ */

static int hook_execute_process(const void *args, void *app, void *sandbox)
{
    char d[192];
    ncm_describe_addr((uintptr_t)app, d, sizeof(d));
    ncm_log("hook: cef_execute_process(app=%s)", app ? d : "(NULL)");
    attach_app(app);
    /* 原函数在子进程里不会返回（它就是子进程的消息循环） */
    return ((ncm_execute_process_fn)g_orig_exec)(args, app, sandbox);
}

/* ── DevTools 注入：改 CefSettings.remote_debugging_port ───────────────────
 * 目的：让页面插件开发能用真正的 Chrome DevTools。
 * 做法：cef_initialize 时校验 CefSettings 布局可信，再写入监听端口。
 *
 * CefSettings（libcef 91）关键偏移：
 *   0   size_t size            40 int multi_threaded_message_loop
 *   48  int windowless_rendering_enabled   52 int command_line_args_disabled
 *   208 int log_severity       268 int remote_debugging_port  ← 目标字段
 * 为防布局不符写坏别的字段，先做一致性校验；不通过就一个字节都不改。 */
#define NCM_CFG_SIZE_OFF          0
#define NCM_CFG_MTML_OFF          40
#define NCM_CFG_WINLESS_OFF       48
#define NCM_CFG_CMD_DISABLED_OFF  52
#define NCM_CFG_LOGSEV_OFF        208
/* ── DevTools 注入：改 CefSettings.remote_debugging_port ───────────────────
 * 结构体偏移不是猜的，是用 dump 出来的实际内容定位的（详见 docs/07 §7.47）：
 *   112 = cache_path   "…\NetEase\CloudMusic\webapp91x64"
 *   192 = user_agent   "Mozilla/5.0 (Windows NT 10.0; WOW64) …"
 *   264 = log_file     "…\NetEase\CloudMusic\web.log"
 *   376 = background_color = 0x100000
 * 由 CEF 的字段顺序（…locales_dir_path, pack_loading_disabled,
 * remote_debugging_port, uncaught_exception_stack_size, background_color）反推：
 *   background_color=376 → uncaught=372 → remote_debugging_port=368.
 * 写入前做三重校验（三个 int 必须为 0，且 background_color 非零），否则一个字节都不改。 */
#define NCM_CFG_SIZE_OFF          0
#define NCM_CFG_PACK_LOAD_OFF     364
#define NCM_CFG_DEVTOOLS_OFF      368
#define NCM_CFG_UNCAUGHT_OFF      372
#define NCM_CFG_BGCOLOR_OFF       376

static int g_devtools_port = 0;             /* 0 = 关闭；偏移未完全验证前保持关闭 */
static volatile LONG g_devtools_done = 0;

static void devtools_patch_settings(const void *settings)
{
    if (!settings || !g_devtools_port) return;
    if (InterlockedExchange(&g_devtools_done, 1)) return;

    const unsigned char *p = (const unsigned char *)settings;
    size_t size = *(const size_t *)(p + NCM_CFG_SIZE_OFF);
    if (size < 380 || size > 4096) {
        ncm_log("settings: size=%llu 不像 CefSettings，放弃", (unsigned long long)size);
        return;
    }

    int packload = *(const int *)(p + NCM_CFG_PACK_LOAD_OFF);
    int port     = *(const int *)(p + NCM_CFG_DEVTOOLS_OFF);
    int uncaught = *(const int *)(p + NCM_CFG_UNCAUGHT_OFF);
    int bgcolor  = *(const int *)(p + NCM_CFG_BGCOLOR_OFF);

    ncm_log("settings: packLoad=%d port=%d uncaught=%d bgcolor=0x%X",
            packload, port, uncaught, bgcolor);

    if (packload != 0 || port != 0 || uncaught != 0 || bgcolor == 0) {
        ncm_log("settings: 字段校验未通过，放弃 DevTools 注入（不改任何字段）");
        return;
    }

    *(int *)(p + NCM_CFG_DEVTOOLS_OFF) = g_devtools_port;
    ncm_log("settings: ✅ 已写入 remote_debugging_port=%d → http://127.0.0.1:%d",
            g_devtools_port, g_devtools_port);
}

static int hook_initialize(void *args, const void *settings, void *app, void *sandbox)
{
    char d[192];
    ncm_describe_addr((uintptr_t)app, d, sizeof(d));
    ncm_log("hook: cef_initialize(app=%s)", app ? d : "(NULL)");
    attach_app(app);
    devtools_patch_settings(settings);
    return ((ncm_cef_initialize_fn)g_orig_init)(args, settings, app, sandbox);
}

/* ── 探针：宿主还导入了哪些 CEF 入口（决定渲染进程里还有哪些注入载体） ────── */
static volatile LONG g_reg_ext_calls = 0;
static volatile LONG g_cur_ctx_calls = 0;
static int g_ctx_tried = 0;
static int g_ctx_dumps = 0;
static void *g_orig_reg_ext = NULL;
static void *g_orig_cur_ctx = NULL;

/** cef_register_extension(name, code, handler)
 *  这是**页面级**注入载体：宿主自己就是靠它把 JS 塞进每个上下文的。
 *  它不依赖任何结构体偏移（不用猜 vtable），所以比内存补丁稳。 */
static void hook_register_extension(const void *name, const void *code, void *handler)
{
    LONG n = InterlockedIncrement(&g_reg_ext_calls);
    if (n <= 2) {
        char cn[256] = "(null)", cc[256] = "";
        if (name) ncm_cefstr_to_utf8(name, cn, sizeof(cn));
        if (code) ncm_cefstr_to_utf8(code, cc, sizeof(cc));
        ncm_log("hook: cef_register_extension 第 %ld 次 name=\"%s\" code=%.160s", n, cn, cc);
    }

    /* 宿主自己的扩展先照原样注册（绝不能挡住它 —— 那是宿主自己的 JS 桥） */
    if (g_orig_reg_ext)
        ((void (*)(const void *, const void *, void *))g_orig_reg_ext)(name, code, handler);

    /* ★ 然后注册我们自己的扩展：这是渲染进程里**唯一实测有效**的页面级注入载体。
     *   实测：渲染进程里 CEF 向 app 要 get_render_process_handler 的次数 = 0，
     *   而 cef_register_extension 被调 1 次、cef_v8context_get_current_context 被调 1039 次。 */
    if (n == 1) {
        ncm_log("→ 这是页面级注入的时机，开始注册自己的扩展");
        ncm_host_register();
    }
}

/** cef_v8context_get_current_context() —— 渲染进程里拿 JS 上下文的入口。
 *  这是**真正可用**的注入跳板：实测渲染进程里它被调 1039 次，
 *  而 app->get_render_process_handler 被调 0 次（docs/07 §7.2.4）。 */
static void ctx_frame_try(void *ctx);   /* 定义见本节后部 */
static void eval_channel_probe(void *ctx);
static void exec_smoke_test(void *ctx);
static void exec_smoke_test2(void *ctx);
static long long v8_raw2(void *obj, int idx, const char *key);
static int  v8_set_key(void *obj, int idx, const char *key, void *val);
static int  v8_has_key(void *obj, int idx, const char *key);
static int  frame_execute_js_u(void *frame, int idx, const char *code, const char *script_url);
static int  ctx_enter(void *ctx);
static void ctx_exit(void *ctx);

/** cef_v8context_get_current_context() —— 渲染进程里拿 JS 上下文的入口。
 *  实测渲染进程里它被调 1039 次，而 app->get_render_process_handler 被调 0 次。 */
/* ══════════════════════════════════════════════════════════════════════════
 * 干净的单次冒烟测试：execute_java_script 到底能不能改页面
 *
 * 背景：7.19 判定"空操作"的那批实验，是在**渲染进程被探针反复搞崩**的环境里做的，
 * 结论不可信。现在探针已关、宿主窗口正常，这里只做**一次**最小验证：
 *   1) 取 frame，读 URL（基线）
 *   2) 执行 `location.hash='ncmexec1'`
 *   3) 再读 URL —— 变了就说明 JS 真的执行了
 * 只调用两次 get_url + 一次 execute_java_script，没有任何遍历/探测，风险极低。
 * 开关：NCM_ENABLE_EXEC_SMOKE（默认开，一次就结束）
 * ══════════════════════════════════════════════════════════════════════════ */
#ifndef NCM_ENABLE_EXEC_SMOKE
#define NCM_ENABLE_EXEC_SMOKE 1
#endif

static int g_smoke_done = 0;

static int frame_url_of(void *frame, char *out, int out_size)
{
    out[0] = 0;
    uintptr_t gf = 0;
    __try { gf = *(uintptr_t *)((uint8_t *)frame + NCM_CEF_BASE_SIZE + (uintptr_t)19 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { gf = 0; }
    if (!ncm_ptr_is_code(gf)) return 0;
    return try_call_get_url(frame, gf, out, out_size) == 1;
}

static void exec_smoke_test(void *ctx)
{
    if (g_smoke_done || !ctx) return;
    g_smoke_done = 1;

    uintptr_t f3 = 0;
    __try { f3 = *(uintptr_t *)((uint8_t *)ctx + NCM_CEF_BASE_SIZE + (uintptr_t)3 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { f3 = 0; }
    if (!ncm_ptr_is_code(f3)) { ncm_trace("冒烟: ctx idx3 不是代码"); return; }

    void *frame = NULL;
    __try { frame = ((void *(*)(void *))f3)(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { frame = NULL; }
    if (!frame) { ncm_trace("冒烟: 拿不到 frame"); return; }

    char before[1024] = {0};
    if (!frame_url_of(frame, before, sizeof(before))) {
        ncm_trace("冒烟: 读不到 frame URL，放弃");
        return;
    }
    ncm_trace("冒烟起点: URL=%s", before);

    /* 只执行这一句；不碰任何全局读取 */
    if (!frame_execute_js_u(frame, 18, "try{location.hash='ncmexec1'}catch(e){}",
                            "orpheus://orpheus/pub/app.html")) {
        ncm_trace("冒烟: execute_java_script 调用抛异常");
        return;
    }
    Sleep(500);

    char after[1024] = {0};
    frame_url_of(frame, after, sizeof(after));
    if (strstr(after, "ncmexec1")) {
        ncm_trace("✅✅ 冒烟通过：execute_java_script 真的执行了（URL=%s）", after);
        /* 通过就立刻清掉 hash，并标记可用 —— 后续可以接真正的载荷注入 */
        frame_execute_js_u(frame, 18, "try{location.hash=''}catch(e){}",
                           "orpheus://orpheus/pub/app.html");
        g_injections++;
    } else {
        ncm_trace("❌ 冒烟失败：hash 未变（URL=%s）—— execute_java_script 确认无效", after);
    }
}
static void *hook_cur_ctx(void)
{
    LONG n = InterlockedIncrement(&g_cur_ctx_calls);
    if (n <= 2) ncm_log("hook: cef_v8context_get_current_context 第 %ld 次被调用", n);

    void *ctx = g_orig_cur_ctx ? ((void *(*)(void))g_orig_cur_ctx)() : NULL;

    if (NCM_ENABLE_CTX_PROBE && ctx && g_ctx_dumps < 2) {
        g_ctx_dumps++;
        uintptr_t cefb = 0; size_t cefsz = 0;
        ncm_module_nolock(L"libcef.dll", &cefb, &cefsz);
        ncm_trace("ctx#%d = %p  libcef_base = %p", g_ctx_dumps, ctx, (void *)cefb);
        for (int i = 0; i < 18; i++) {
            void *p = NULL;
            __try { p = ((void **)((uint8_t *)ctx + NCM_CEF_BASE_SIZE))[i]; }
            __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            ncm_trace("  v8ctx[%2d] +0x%02X = %p", i,
                      (unsigned)(NCM_CEF_BASE_SIZE + (uintptr_t)i * 8), p);
        }
    }

    /* 页面注入状态机：每 10 次调用推进一格（给消息循环留出执行机会） */
    /* 干净的单次冒烟：确认 execute_java_script 是否真的无效 */
    if (NCM_ENABLE_EXEC_SMOKE && ctx && n == 6) exec_smoke_test(ctx);
    if (NCM_ENABLE_EXEC_SMOKE2 && ctx && n == 9) exec_smoke_test2(ctx);

    if (NCM_ENABLE_RENDERER_PROBE && ctx && (n % 20) == 7) eval_channel_probe(ctx);

    if (NCM_ENABLE_CTX_PROBE && ctx && !g_injections && (n % 100) == 1) ctx_eval_inject(ctx);

    return ctx;
}

/* 跨调用状态机：CEF 的 execute_java_script 很可能把任务投递到消息循环，
 * 而我们正卡在 V8 回调里 —— 任务要等我们返回后才执行，所以"当场验证"永远失败。
 * 于是把「投递 → 等待 → 复查」拆到多次 hook 调用里完成。 */
typedef struct {
    int   stage;          /* 0 未开始 / 1 已投递等复查 / 2 成功 / 3 放弃 */
    int   cand_i;
    int   wait;
    void *frame;
    void *global;
    int   has_idx;
    int   set_idx;
    char  key[64];
    int   url_idx;        /* 记下 get_url 槽位，方便复查 */
} ncm_probe_state;

static int g_frame_inject_done = 0;
static ncm_probe_state g_ps;
static int g_ctxframe_tried = 0;
static int g_size_skips = 0;   /* 跳过 0x0 隐藏文档的次数（诊断用） */

static int g_nav_tested = 0;
static int g_exec_channel_ok = 0;   /* load_url(javascript:) 是否可用 */
/** 调 (obj, &keystring) 形态的方法并返回**原始 64 位返回值**。
 *  用来区分"返回布尔 0/1"和"返回指针"——指针被截断成 int 后也是非 0，
 *  只用 int 判定会把 get_value_bykey 误认成 has_value_bykey（踩过）。 */
static long long v8_raw2(void *obj, int idx, const char *key)
{
    wchar_t w[160];
    if (MultiByteToWideChar(CP_UTF8, 0, key, -1, w, 160) <= 1) return -1;
    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = wcslen(w);
    cs.dtor = NULL;
    uintptr_t fn = 0;
    __try { fn = *(uintptr_t *)((uint8_t *)obj + NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
    if (!ncm_ptr_is_code(fn)) return -2;
    long long r = -3;
    __try { r = ((long long (*)(void *, const cef_string_t *))fn)(obj, &cs); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = -3; }
    return r;
}

/** 取「某个 frame 自己的」v8 上下文里的 global。
 *  为什么要这个：execute_java_script 作用在 frame 的文档上，而 hook 拿到的 ctx
 *  可能是**另一个**上下文（同一渲染进程里多个 frame）。实测混用会导致
 *  「写进 A、读 B」—— 于是尺寸/可见性读数自相矛盾。 */
static void *frame_global(void *frame, int *has_idx_out)
{
    if (!frame) return NULL;

    uintptr_t cf = 0;
    __try { cf = *(uintptr_t *)((uint8_t *)frame + NCM_CEF_BASE_SIZE + (uintptr_t)21 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { cf = 0; }
    if (!ncm_ptr_is_code(cf)) return NULL;

    void *ctx2 = NULL;
    __try { ctx2 = ((void *(*)(void *))cf)(frame); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ctx2 = NULL; }
    if (!ctx2) return NULL;

    uintptr_t g4 = 0;
    __try { g4 = *(uintptr_t *)((uint8_t *)ctx2 + NCM_CEF_BASE_SIZE + (uintptr_t)4 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g4 = 0; }
    if (!ncm_ptr_is_code(g4)) return NULL;

    void *global2 = NULL;
    __try { global2 = ((void *(*)(void *))g4)(ctx2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { global2 = NULL; }
    if (!global2) return NULL;

    if (has_idx_out) {
        int hi = -1;
        for (int i = 20; i <= 44; i++) {
            long long a = v8_raw2(global2, i, "channel");
            long long b = v8_raw2(global2, i, "zz_ncm_absent_9f");
            if (a == 1 && b == 0) { hi = i; break; }
        }
        *has_idx_out = hi;
    }
    return global2;
}
static int ps_resolve(void *ctx, const char *page_url)
{
    uintptr_t f3 = 0;
    __try { f3 = *(uintptr_t *)((uint8_t *)ctx + NCM_CEF_BASE_SIZE + (uintptr_t)3 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!ncm_ptr_is_code(f3)) return 0;

    void *frame = NULL;
    __try { frame = ((void *(*)(void *))f3)(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { frame = NULL; }
    if (!frame) return 0;

    uintptr_t g4 = 0;
    __try { g4 = *(uintptr_t *)((uint8_t *)ctx + NCM_CEF_BASE_SIZE + (uintptr_t)4 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    void *global = NULL;
    if (ncm_ptr_is_code(g4)) {
        __try { global = ((void *(*)(void *))g4)(ctx); }
        __except (EXCEPTION_EXECUTE_HANDLER) { global = NULL; }
    }
    if (!global) { ncm_trace("ps_resolve: get_global 拿不到"); return 0; }

    /* ══ 关键：区分 has_value_bykey 和 delete_value_bykey ══
     * 两者签名和返回值语义**完全相同**（key 存在→1、不存在→0），
     * 双 key 判别根本分不开。实测就是这样把 delete 当成了 has，
     * 于是"写进去马上被删"，所有验证全部失效。
     * 正确判据：对同一个存在的 key **连调两次** ——
     *   has    → 1, 1
     *   delete → 1, 0（第一次就把 key 删了）
     * 判完之后再用 set 把 key 写回去，确保后续步骤成立。 */
    int has_idx = -1, set_idx = -1;
    for (int sc = 28; sc <= 34 && has_idx < 0; sc++) {
        char key[64];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "__ncm_pair_%d", sc);
        void *val = ncm_create_js_string("1");
        if (!val) continue;

        /* set 候选：写进去（写完还不知道 has 在哪，只能先写） */
        if (!v8_set_key(global, sc, key, val)) continue;

        for (int hc = 21; hc <= 27; hc++) {
            int a = v8_has_key(global, hc, key);
            int b = v8_has_key(global, hc, key);
            if (a == 1 && b == 1) {
                int c = v8_has_key(global, hc, key);
                if (c != 1) continue;               /* 不稳定，跳过 */
                has_idx = hc;
                set_idx = sc;
                break;
            }
        }
        if (has_idx < 0) {
            /* 这个 set 候选没成功，key 可能根本没写进去 —— 换下一个 */
            continue;
        }
    }

    if (has_idx < 0) {
        ncm_trace("ps_resolve ❌ 没找到 has/set 槽位对");
        return 0;
    }

    g_ps.frame = frame;
    g_ps.global = global;
    g_ps.has_idx = has_idx;
    g_ps.set_idx = set_idx;
    ncm_trace("probe 就绪：frame=%p global=%p has=%d set=%d page=%s",
              frame, global, has_idx, set_idx, page_url);
    return 1;
}

/** 投递一次 delete 探针；返回 1 表示已投递。 */
static void *g_probe_ctx2 = NULL;

/** 找一个能读出值的槽位：写一个已知字符串，再用候选 get_value_bykey 读回比对。 */
static int find_get_key_idx(void *global, int set_idx, int has_idx)
{
    static int cached = -2;
    if (cached > 0) return cached;          /* 只缓存成功结果，失败可重试 */

    const char *key = "__ncm_get_probe";
    void *val = ncm_create_js_string("NCM_MAGIC_7f3a");
    if (!val) { ncm_trace("  回读：造不出测试字符串"); return -1; }
    if (!v8_set_key(global, set_idx, key, val)) { ncm_trace("  回读：写入失败"); return -1; }
    if (v8_raw2(global, has_idx, key) != 1) { ncm_trace("  回读：写入后 has != 1"); return -1; }

    wchar_t w[160];
    MultiByteToWideChar(CP_UTF8, 0, key, -1, w, 160);
    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = wcslen(w);
    cs.dtor = NULL;

    /* get_value_bykey 在 has 之后不远处；按「读出来的文本 == 我们写进去的文本」自证。
     * 注意 v == (void*)1 这类小整数要排除，它们不是对象。 */
    for (int i = has_idx + 1; i <= has_idx + 12; i++) {
        uintptr_t fn = 0;
        __try { fn = *(uintptr_t *)((uint8_t *)global + NCM_CEF_BASE_SIZE + (uintptr_t)i * 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!ncm_ptr_is_code(fn)) continue;
        void *v = NULL;
        __try { v = ((void *(*)(void *, const cef_string_t *))fn)(global, &cs); }
        __except (EXCEPTION_EXECUTE_HANDLER) { v = NULL; }
        if (!v || (uintptr_t)v < 0x10000) continue;
        char got[128] = {0};
        int okv = ncm_host_value_to_utf8(v, got, sizeof(got));
        if (okv && strcmp(got, "NCM_MAGIC_7f3a") == 0) {
            ncm_trace("  get_value_bykey = idx%d（读回文本一致）", i);
            return cached = i;
        }
    }
    ncm_trace("  回读：has_idx+1..+12 里没有能读出已知值的槽位");
    return -1;
}

/** 投递一次值改写探针：原生写 "1"，JS 改成 "2"，稍后读回比对。 */
static int ps_dispatch(int idx, int vehicle)
{
    /* JS 负责**新建**一个 key；原生只用 has(idx23) 看它有没有出现。
     * 这样完全不依赖 get_value_bykey 的回读能力（那条路实测不通）。 */
    _snprintf_s(g_ps.key, sizeof(g_ps.key), _TRUNCATE, "__ncm_js_%d_%d", vehicle, idx);

    char js[192];
    _snprintf_s(js, sizeof(js), _TRUNCATE,
                "try{%s=1}catch(e){}try{window.%s=1}catch(e){}", g_ps.key, g_ps.key);

    int ok = 0;
    if (vehicle == 0) {
        ok = frame_execute_js_u(g_ps.frame, idx, js, "orpheus://orpheus/pub/app.html");
    } else {
        if (!g_probe_ctx2) {
            uintptr_t cf = 0;
            __try { cf = *(uintptr_t *)((uint8_t *)g_ps.frame + NCM_CEF_BASE_SIZE + (uintptr_t)21 * 8); }
            __except (EXCEPTION_EXECUTE_HANDLER) { cf = 0; }
            if (ncm_ptr_is_code(cf)) {
                __try { g_probe_ctx2 = ((void *(*)(void *))cf)(g_ps.frame); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_probe_ctx2 = NULL; }
            }
        }
        if (!g_probe_ctx2) return 0;
        int entered = ctx_enter(g_probe_ctx2);
        int r = ctx_eval_raw(g_probe_ctx2, idx, js, NULL, 0);
        if (entered) ctx_exit(g_probe_ctx2);
        ok = (r != 0);
    }
    if (!ok) return 0;
    g_ps.wait = 8;
    return 1;
}

/** 读回标记值，判断 JS 是否把它改成了 "2"。 */
static int ps_check_value(void)
{
    /* 1 = JS 真的把这个 key 建出来了 */
    return (v8_raw2(g_ps.global, g_ps.has_idx, g_ps.key) == 1) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * eval 通道测试（用 hash 读回当唯一裁判）
 *
 * 背景：渲染进程侧 frame 的 execute_java_script(idx18) 实测是空操作
 * （7.19 的导航测试：location.href 都没变）。参照项目 chromatic 的做法是
 * 根本不走 CEF 的 JS API，而是在进程内自己 hook —— 说明这条路即便不通用，
 * 也还有 v8context 自己的 eval 可以试。
 *
 * 判据只用一个：让 JS 改 location.hash，再用 frame 的 get_url 读回。
 * 哈希变了才算通，任何布尔标记都不作数（7.19 的教训）。
 * ══════════════════════════════════════════════════════════════════════════ */
static int g_eval_tested = 0;

static int read_frame_url(void *frame, char *out, int out_size)
{
    if (out_size > 0) out[0] = 0;
    uintptr_t gf = 0;
    __try { gf = *(uintptr_t *)((uint8_t *)frame + NCM_CEF_BASE_SIZE + (uintptr_t)19 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { gf = 0; }
    if (!ncm_ptr_is_code(gf)) return 0;
    return try_call_get_url(frame, gf, out, out_size) == 1;
}

static void eval_channel_probe(void *ctx)
{
    if (g_eval_tested || !ctx) return;
    g_eval_tested = 1;

    /* 先拿 frame，只为了读 URL 和后面注入 */
    uintptr_t f3 = 0;
    __try { f3 = *(uintptr_t *)((uint8_t *)ctx + NCM_CEF_BASE_SIZE + (uintptr_t)3 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { f3 = 0; }
    if (!ncm_ptr_is_code(f3)) { ncm_trace("eval 测试：ctx idx3 不是代码"); return; }

    void *frame = NULL;
    __try { frame = ((void *(*)(void *))f3)(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { frame = NULL; }
    if (!frame) { ncm_trace("eval 测试：拿不到 frame"); return; }

    char before[1024] = {0};
    read_frame_url(frame, before, sizeof(before));
    ncm_trace("eval 测试起点 URL = %s", before);

    /* enter/exit 的候选（is_valid=1 实测在 idx1，get_frame=3、get_global=4，
     * 所以 enter/exit 很可能在 5/6；再带上 1/2 兜底） */
    static const int pairs[][2] = { {5, 6}, {6, 5}, {1, 6}, {5, 1}, {0, 0} };
    static const int evals[] = { 7, 8, 9, 6, 10 };

    for (int p = 0; p < 5 && g_injections == 0; p++) {
        for (int e = 0; e < 5; e++) {
            int ei = evals[e];
            int entered = 0;
            if (pairs[p][0] != 0) entered = ctx_call_int(ctx, pairs[p][0]);

            char js[96];
            _snprintf_s(js, sizeof(js), _TRUNCATE,
                        "try{location.hash='ncm_ev_%d_%d'}catch(e){}", pairs[p][0], ei);
            int r = ctx_eval_raw(ctx, ei, js, NULL, 0);

            if (entered) ctx_call_int(ctx, pairs[p][1]);
            Sleep(300);

            char after[1024] = {0};
            read_frame_url(frame, after, sizeof(after));
            int hit = (strstr(after, "ncm_ev_") != NULL);
            ncm_trace("eval 测试：enter=%d eval=idx%d 返回=%d → URL 变化=%d",
                      pairs[p][0], ei, r, hit);
            if (hit) {
                ncm_trace("✅✅ eval 通道可用：enter=%d exit=%d eval=idx%d",
                          pairs[p][0], pairs[p][1], ei);
                /* 通了就顺手注入真载荷 */
                char *bundle = ncm_bundle_build();
                if (bundle) {
                    int en2 = (pairs[p][0] != 0) ? ctx_call_int(ctx, pairs[p][0]) : 0;
                    int r2 = ctx_eval_raw(ctx, ei, bundle, NULL, 0);
                    if (en2) ctx_call_int(ctx, pairs[p][1]);
                    ncm_trace("载荷经 eval 投递：返回=%d，%zu 字节", r2, strlen(bundle));

                    Sleep(600);
                    char fin[1024] = {0};
                    read_frame_url(frame, fin, sizeof(fin));
                    ncm_trace("注入后 URL = %s", fin);
                    g_injections++;
                    free(bundle);
                }
                return;
            }
        }
    }
    ncm_trace("eval 测试：5 组 enter×5 个 eval 槽位全部无效 —— v8context::eval 也不通");
}
static void ctx_frame_try(void *ctx)
{
    if (!ctx || g_ps.stage == 3) return;


    /* ★ 页面重载 / SPA 路由切换会把手挂的 DOM 一起冲掉。
     *   注入完成后（stage==2）持续守护：页面里没有 __NCM__ 就复位并重新注入。 */
    if (g_ps.stage == 2) {
        if (v8_raw2(g_ps.global, g_ps.has_idx, "__NCM__") == 1) return;
        ncm_trace("页面里 __NCM__ 不见了（重载/路由切换）→ 重新注入");
        g_ps.stage = 0;
        g_ps.wait = 0;
        g_ps.cand_i = 0;
        g_injections = 0;
    }
    if (g_injections) return;

    static const int ecands[] = { 18 };   /* 实测确认：frame idx18 = execute_java_script */
    static const int vcands[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14 };
    const int NEXEC = 1, NEVAL = 0;   /* eval 通道暂不需要：frame idx18 已确认可用 */

    if (g_ps.stage == 0) {
        if (!ps_resolve(ctx, "")) return;

        /* ★ 实测：CEF 里存在 0×0 的隐藏 browser 文档，NCM 的页面 JS 也会在这些
         *   上下文里调 get_current_context。之前就是注入了那个隐藏文档 ——
         *   DOM 检查全绿（元素真的在树里、有尺寸），但屏幕上什么也没有。
         *   所以先探尺寸：不像主窗口（< 800×600）就放弃这个文档、等下一个 ctx。 */
        {
            /* ★ 读数通道：让页面把状态编码进 location.hash，再用 frame 的 get_url 读回。
             *   为什么不用 has_value_bykey：它在「另一个上下文」里会命中错误的槽位
             *   （实测读回过 26 = delete_value_bykey），导致所有布尔量假阳性/假阴性。
             *   hash → get_url 这条链是已经验证可靠的（frame idx19）。
             *   编码：ncmr<visible><big><top><banner><iframe><badge>，每位 0/1。 */
        /* ★ 一次性的"到底能不能执行 JS"判定（不可辩驳）：
         *   让 frame 真的导航到带 #ncmnav 的地址，再用 get_url 读回。
         *   hash 赋值没反应已经说明问题，这个测试用来把结论钉死。 */
        if (!g_nav_tested) {
            g_nav_tested = 1;
            char jsn[256];
            _snprintf_s(jsn, sizeof(jsn), _TRUNCATE,
                        "location.href='orpheus://orpheus/pub/app.html#ncmnav';");
            frame_execute_js_u(g_ps.frame, ecands[0], jsn, "orpheus://orpheus/pub/app.html");
            Sleep(800);
            uintptr_t gu = 0;
            __try { gu = *(uintptr_t *)((uint8_t *)g_ps.frame + NCM_CEF_BASE_SIZE + (uintptr_t)19 * 8); }
            __except (EXCEPTION_EXECUTE_HANDLER) { gu = 0; }
            char uu[1024] = "";
            if (ncm_ptr_is_code(gu)) try_call_get_url(g_ps.frame, (uintptr_t)gu, uu, sizeof(uu));
            ncm_trace("导航测试：执行 location.href=#ncmnav 后 URL = %s", uu);
            ncm_trace("导航测试结论：%s", strstr(uu, "ncmnav") ? "JS 真的执行了" : "JS 未生效（execute_java_script 是空操作）");
        }

            char js[400];
            _snprintf_s(js, sizeof(js), _TRUNCATE,
                        "try{var S='ncmr';"
                        "S+=(document.visibilityState==='visible')?'1':'0';"
                        "S+=((window.innerWidth>=800&&window.innerHeight>=600)?'1':'0');"
                        "S+=((window.top===window.self)?'1':'0');"
                        "S+=(document.getElementById('ncm-inject-banner')?'1':'0');"
                        "S+=((document.querySelectorAll('iframe').length>0)?'1':'0');"
                        "S+=(document.getElementById('ncm-inject-badge')?'1':'0');"
                        "location.hash=S;}catch(e){}");
            frame_execute_js_u(g_ps.frame, ecands[0], js, "orpheus://orpheus/pub/app.html");
            Sleep(250);

            uintptr_t gfu = 0;
            __try { gfu = *(uintptr_t *)((uint8_t *)g_ps.frame + NCM_CEF_BASE_SIZE + (uintptr_t)19 * 8); }
            __except (EXCEPTION_EXECUTE_HANDLER) { gfu = 0; }
            char rep[1024] = "";
            if (ncm_ptr_is_code(gfu)) try_call_get_url(g_ps.frame, (uintptr_t)gfu, rep, sizeof(rep));

            const char *tok = strstr(rep, "#ncmr");
            if (!tok) tok = strstr(rep, "ncmr");
            int vis = 0, big = 0, top = 0, ban = 0, ifr = 0, bdg = 0;
            if (tok && strlen(tok) >= 10) {
                const char *p = strstr(tok, "ncmr") + 4;
                vis = p[0] == '1'; big = p[1] == '1'; top = p[2] == '1';
                ban = p[3] == '1'; ifr = p[4] == '1'; bdg = p[5] == '1';
            }
            ncm_trace("frame 自述：%s → visible=%d 大文档=%d 顶层=%d 横幅=%d iframe=%d 徽标=%d",
                      tok ? tok : "(未读到)", vis, big, top, ban, ifr, bdg);

            if (!big) {
                g_size_skips++;
                if (g_size_skips <= 5 || (g_size_skips % 50) == 0)
                    ncm_trace("跳过 #%d：不是主窗口尺寸（%s）", g_size_skips, rep);
                g_ps.stage = 0;
                g_ps.frame = NULL;
                return;
            }
            ncm_trace("目标文档尺寸合格 → 开始注入（%s）", rep);
            /* 清掉刚才写进去的 hash，别把页面 URL 弄脏 */
            frame_execute_js_u(g_ps.frame, ecands[0], "try{location.hash=''}catch(e){}",
                               "orpheus://orpheus/pub/app.html");
        }

        g_ps.cand_i = 0;
        if (!ps_dispatch(ecands[0], 0)) { g_ps.stage = 3; ncm_trace("首轮投递失败"); return; }
        ncm_trace("已投递 JS 探针（idx%d），等待消息循环执行……", ecands[0]);
        g_ps.stage = 1;
        return;
    }

    if (g_ps.stage == 1) {
        if (g_ps.wait > 0) { g_ps.wait--; return; }

        int changed = ps_check_value();
        ncm_trace("复查 vehicle=%d cand=%d：值已变=%d（1 = JS 真的执行了）", (g_ps.cand_i < NEXEC ? 0 : 1), g_ps.cand_i, changed);
        if (changed == 1) {
            g_ps.stage = 2;
            ncm_trace("✅✅ 页面 JS 执行确认：vehicle=%d cand=%d", (g_ps.cand_i < NEXEC ? 0 : 1), g_ps.cand_i);

            char *bundle = ncm_bundle_build();
            if (bundle) {
                /* 记下「注入到哪一页」——多窗口/多渲染进程时，这条是唯一能对上号的证据 */
                {
                    uintptr_t gf2 = 0;
                    __try { gf2 = *(uintptr_t *)((uint8_t *)g_ps.frame + NCM_CEF_BASE_SIZE + (uintptr_t)19 * 8); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { gf2 = 0; }
                    char page[1024] = "(unknown)";
                    if (ncm_ptr_is_code(gf2)) try_call_get_url(g_ps.frame, gf2, page, sizeof(page));
                    ncm_trace("注入目标页面 = %s", page);
                }
                if (frame_execute_js_u(g_ps.frame, ecands[g_ps.cand_i], bundle,
                                       "orpheus://orpheus/pub/app.html")) {
                    g_injections++;
                    g_frame_inject_done = 1;
                    ncm_trace("🎉 载荷已投递（%zu 字节）", strlen(bundle));

                    /* ── 注入后验证：全部走「JS 建 key → 原生 has 检查」这一套（已自证可靠） ── */
                    Sleep(800);
                    int ok_ncm  = (v8_raw2(g_ps.global, g_ps.has_idx, "__NCM__") == 1);
                    int ok_host = (v8_raw2(g_ps.global, g_ps.has_idx, "__NCM_HOST__") == 1);
                    ncm_trace("验证：__NCM__=%d  __NCM_HOST__=%d", ok_ncm, ok_host);

                    const char *jsDom =
                        "try{__ncm_dom_ok=(document.getElementById('ncm-inject-badge')?1:0);}catch(e){}"
                        "try{__ncm_doc_ok=(document.body?1:0);}catch(e){}";
                    frame_execute_js_u(g_ps.frame, ecands[g_ps.cand_i], jsDom,
                                       "orpheus://orpheus/pub/app.html");
                    Sleep(400);
                    int dom_ok = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_dom_ok") == 1);
                    int doc_ok = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_doc_ok") == 1);
                                        int demo_ok = (v8_raw2(g_ps.global, g_ps.has_idx, "__DEMO_RAN") == 1);
                    int ui_ok   = (v8_raw2(g_ps.global, g_ps.has_idx, "__UI_RAN") == 1);
                    int ipc_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__IPC_RAN") == 1);
                    int hook_ok = (v8_raw2(g_ps.global, g_ps.has_idx, "__HOOK_RAN") == 1);
                    int inj_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__DEMO_INJECT_RAN") == 1) ||
                                  (v8_raw2(g_ps.global, g_ps.has_idx, "__UI_DEMO_INJECT") == 1);
                    ncm_trace("验证：document=%d 徽标=%d 插件脚本=%d | ui=%d ipc=%d hook=%d injects=%d",
                              doc_ok, dom_ok, demo_ok, ui_ok, ipc_ok, hook_ok, inj_ok);

                    /* 注意：has_value_bykey 只判断「键存在」，不判断值。
                     * 所以这里全部改成「条件成立才建键」—— 否则 __x=0 也会被读成 1，
                     * 之前那批验证就是因为这个原因全部假阳性（踩过）。 */
                    const char *jsVis =
                        "try{if(document.visibilityState==='visible')window.__ncm_vis_ok=1;}catch(e){}"
                        "try{var W=window.innerWidth,HH=window.innerHeight;"
                        "if(W>=800&&HH>=600)window.__ncm_big_ok=1;"
                        "window['__ncm_wpx_'+(Math.round(W/100)*100)]=1;"
                        "window['__ncm_hpx_'+(Math.round(HH/100)*100)]=1;}catch(e){}"
                        "try{if(window.top===window.self)window.__ncm_top_ok=1;}catch(e){}"
                        "try{if(document.querySelectorAll('iframe').length>0)window.__ncm_ifr_ok=1;}catch(e){}"
                        "try{var b=document.getElementById('ncm-inject-banner');"
                        "var rb=b?b.getBoundingClientRect():null;"
                        "if(rb&&rb.width>0&&rb.top<50)window.__ncm_banner_ok=1;"
                        "var hit=rb?document.elementFromPoint((rb.left+rb.right)/2,(rb.top+rb.bottom)/2):null;"
                        "if(hit===b)window.__ncm_hit_ok=1;}catch(e){}"
                        "try{var e2=document.getElementById('ncm-inject-badge');"
                        "var r2=e2?e2.getBoundingClientRect():null;"
                        "if(r2&&r2.width>0&&r2.height>0)window.__ncm_box_ok=1;"
                        "var h2=r2?document.elementFromPoint((r2.left+r2.right)/2,(r2.top+r2.bottom)/2):null;"
                        "if(h2===e2)window.__ncm_hit2_ok=1;}catch(e){}";
                    frame_execute_js_u(g_ps.frame, ecands[g_ps.cand_i], jsVis,
                                       "orpheus://orpheus/pub/app.html");
                    Sleep(300);
                    int vis_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_vis_ok") == 1);
                    int big_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_big_ok") == 1);
                    int top_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_top_ok") == 1);
                    int ifr_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_ifr_ok") == 1);
                    int ban_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_banner_ok") == 1);
                    int box_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_box_ok") == 1);
                    int hit_ok  = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_hit_ok") == 1);
                    int hit2_ok = (v8_raw2(g_ps.global, g_ps.has_idx, "__ncm_hit2_ok") == 1);
                    int wpix = -1, hpix = -1;
                    for (int px = 0; px <= 3000; px += 100) {
                        char k[40];
                        _snprintf_s(k, sizeof(k), _TRUNCATE, "__ncm_wpx_%d", px);
                        if (v8_raw2(g_ps.global, g_ps.has_idx, k) == 1) { wpix = px; break; }
                    }
                    for (int px = 0; px <= 2000; px += 100) {
                        char k[40];
                        _snprintf_s(k, sizeof(k), _TRUNCATE, "__ncm_hpx_%d", px);
                        if (v8_raw2(g_ps.global, g_ps.has_idx, k) == 1) { hpix = px; break; }
                    }
                    ncm_trace("实测：尺寸≈%dx%d visible=%d 大文档=%d 顶层=%d iframe=%d | 横幅=%d 徽标=%d 横幅命中=%d 徽标命中=%d",
                              wpix, hpix, vis_ok, big_ok, top_ok, ifr_ok, ban_ok, box_ok, hit_ok, hit2_ok);
                }
                free(bundle);
            }
            return;
        }

        g_ps.cand_i++;
        int vehicle = (g_ps.cand_i < NEXEC) ? 0 : 1;
        int slot = (vehicle == 0) ? g_ps.cand_i : (g_ps.cand_i - NEXEC);
        if (vehicle == 1 && slot >= NEVAL) {
            g_ps.stage = 3;
            ncm_trace("❌ frame(7) + eval(9) 共 16 个候选都没能让标记消失");
            return;
        }
        int idx = (vehicle == 0) ? ecands[slot] : vcands[slot];
        if (!ps_dispatch(idx, vehicle)) {
            g_ps.wait = 0;
            ncm_trace("  候选 vehicle=%d idx=%d 投递失败，跳过", vehicle, idx);
            return;
        }
        ncm_trace("  已投递 vehicle=%d idx=%d", vehicle, idx);
    }
}


/* ══════════════════════════════════════════════════════════════════════════
 * DllMain 内抢装 —— 真正的零竞争路径
 * ══════════════════════════════════════════════════════════════════════════ */

static int g_dllmain_armed = -1;   /* -1 = 没试过 */

/**
 * 为什么必须在 DllMain 里做（而不是起线程）：
 *
 * 第一次实机跑：IAT 补丁确实装上了（RVA 与导出表完全吻合），但子进程里
 * cef_execute_process **早在我们装补丁之前就被调过** —— 补丁=2 却一条 hook 记录都没有。
 * 我们随后把安装提前到线程启动后 3ms（耗时实测 0ms），**依然输**。
 * 原因不是代码慢，而是**线程调度**：加载器一放锁，主线程立刻往下走，
 * 我们的新线程什么时候被 OS 排上是不确定的。
 *
 * 而 cloudmusic.dll 的导入描述符顺序给了我们一条零竞争的路：
 *     #12 libcef.dll      ← 我们 hook 的 IAT 在这里
 *     ...
 *     #33 MSIMG32.dll     ← 我们的 DllMain 在这里才跑
 * 加载器是**按描述符顺序逐个解析并回填 IAT** 的，所以轮到 MSIMG32 时，
 * libcef 那一批槽位**早就填好了真身地址**。
 * ⇒ 在 DllMain 里直接换掉即可，既抢在 cef_execute_process 之前，
 *   也不需要 GetProcAddress（槽位里就是真身），更不会被加载器后写覆盖。
 *
 * 约束：DllMain 内不能开日志、不能调加载器、不能碰 CRT 堆。
 * 所以这里只做"读 PEB + 改一个指针"，结果只写全局变量，由 worker 线程记日志。
 */

/* ── 统一补丁表 ────────────────────────────────────────────────────────────
 * 把"打哪几个 CEF 导出、换成本地哪个函数、原地址存哪"集中描述一遍，
 * DllMain 抢装和 worker 补装共用同一段逻辑（都走无锁 PEB 走链，不碰 psapi/锁）。 */
typedef struct {
    const char *fn;      /* libcef.dll 导出名 */
    void       *hook;    /* 我们的替换 */
    void      **orig;    /* 原地址存这里（非 NULL 表示已装） */
    void      **slot;    /* 槽位指针存这里（*slot 就是 IAT 槽地址），供 verify 用 */
    const char *why;
} ncm_patch;

/* ══════════════════════════════════════════════════════════════════════════
 * 浏览器进程路线：cef_browser_host_create_browser_sync → cef_browser_t
 *                → get_main_frame → execute_java_script
 *
 * 为什么换到浏览器进程（docs/07 §7.4.1）：实测证明渲染进程里 CEF 根本不读
 * cef_app_t 的任何成员，所以 on_context_created 这条路在本版本是死的。
 * 而浏览器进程里**确实存在** cef_browser_t，CEF 也允许在浏览器进程对 frame
 * 执行 JS（内部会走 IPC 发给渲染进程）。
 *
 * 依旧不猜偏移：browser 的方法表按「调用后能否返回一个能给出 URL 的 frame」
 * 反查，frame 的 get_url 按「返回串像不像 URL」反查。
 * execute_java_script 的槽位也用行为验证：执行 `location.hash=...` 之后
 * 再读一次 get_url，URL 变了才算成功 —— 闭环自证，不依赖任何日志。
 * ══════════════════════════════════════════════════════════════════════════ */

#ifndef NCM_ENABLE_BROWSER_INJECT
#define NCM_ENABLE_BROWSER_INJECT 0
#endif

typedef void *(*ncm_create_browser_sync_fn)(const void *window_info, const void *client,
                                            const void *url, const void *settings,
                                            const void *extra_info, const void *request_ctx);
static void *g_orig_create_sync = NULL;
static void *g_orig_create_async = NULL;
static void *g_orig_scheme_factory = NULL;   /* cef_register_scheme_handler_factory 原地址 */
static volatile LONG g_create_calls = 0;
static int g_browser_frame_idx = -1;     /* cef_browser_t 里 get_main_frame 的槽位 */
static int g_browser_dumped = 0;
static int g_frame_dumped = 0;
static void *g_browser = NULL;        /* create_browser_sync 拿到的 cef_browser_t* */

/** 在 frame 上执行 JS（调用者保证 g_frame_execjs_off 已定）。 */
static int frame_execute_js_u(void *frame, int idx, const char *code, const char *script_url)
{
    void *fn = ((void **)((uint8_t *)frame + NCM_CEF_BASE_SIZE))[idx];
    if (!fn) return 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, code, -1, NULL, 0);
    if (wlen <= 1) return 0;
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return 0;
    MultiByteToWideChar(CP_UTF8, 0, code, -1, w, wlen);

    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = (size_t)(wlen - 1);
    cs.dtor = NULL;

    /* script_url 传页面自己的 URL：实测传 ncm://inj 这种非页面 scheme 时 CEF 可能不执行。 */
    static const uint16_t kUrl[] = { 'n','c','m',':','/','/','i','n','j',0 };
    cef_string_t us;
    us.str = (uint16_t *)kUrl;
    us.length = 9;
    us.dtor = NULL;

    wchar_t *wurl = NULL;
    if (script_url && script_url[0]) {
        int wl = MultiByteToWideChar(CP_UTF8, 0, script_url, -1, NULL, 0);
        if (wl > 1) {
            wurl = (wchar_t *)malloc((size_t)wl * sizeof(wchar_t));
            if (wurl) {
                MultiByteToWideChar(CP_UTF8, 0, script_url, -1, wurl, wl);
                us.str = (uint16_t *)wurl;
                us.length = (size_t)(wl - 1);
            }
        }
    }

    __try {
        ((void (*)(void *, const cef_string_t *, const cef_string_t *, int))fn)(frame, &cs, &us, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        free(w);
        free(wurl);
        return 0;
    }
    free(w);
    free(wurl);
    return 1;
}
/* ══════════════════════════════════════════════════════════════════════════
 * 第二条执行通道测试：frame->load_url("javascript:...")
 *
 * 为什么值得试：CEF 里 `javascript:` URL 是一次**导航**，走的是
 * 「导航 → V8 执行」路径，与 execute_java_script 完全不是同一条实现。
 * 实测 execute_java_script(idx18) 无效果，但 load_url 可能仍然可用。
 *
 * 判据依旧是唯一可信的那个：让页面改 location.hash，再读 frame URL。
 * 只发一次导航，风险低（不支持的实现会当作无效 URL 忽略）。
 * ══════════════════════════════════════════════════════════════════════════ */
#ifndef NCM_ENABLE_EXEC_SMOKE2
#define NCM_ENABLE_EXEC_SMOKE2 1
#endif

static int g_smoke2_done = 0;

/* 调 frame 的单字符串方法（load_url / load_string 这类） */
static int frame_call_str_u(void *frame, int idx, const char *u8)
{
    if (!frame) return 0;
    uintptr_t fn = 0;
    __try { fn = *(uintptr_t *)((uint8_t *)frame + NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fn = 0; }
    if (!ncm_ptr_is_code(fn)) return 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8, -1, NULL, 0);
    if (wlen <= 1) return 0;
    wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) return 0;
    MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, wlen);

    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = (size_t)(wlen - 1);
    cs.dtor = NULL;

    __try {
        ((void (*)(void *, const cef_string_t *))fn)(frame, &cs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        free(w);
        return 0;
    }
    free(w);
    return 1;
}

static void exec_smoke_test2(void *ctx)
{
    if (g_smoke2_done || !ctx) return;
    g_smoke2_done = 1;

    uintptr_t f3 = 0;
    __try { f3 = *(uintptr_t *)((uint8_t *)ctx + NCM_CEF_BASE_SIZE + (uintptr_t)3 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { f3 = 0; }
    if (!ncm_ptr_is_code(f3)) return;

    void *frame = NULL;
    __try { frame = ((void *(*)(void *))f3)(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { frame = NULL; }
    if (!frame) { ncm_trace("load_url 测试: 拿不到 frame"); return; }

    char before[1024] = {0};
    frame_url_of(frame, before, sizeof(before));

    /* load_url 的槽位：header 顺序 load_url(12) + 实测偏移 4 = 16 */
    int sent = frame_call_str_u(frame, 16, "javascript:location.hash='ncmjs1';void(0)");
    ncm_trace("load_url 测试: 发送 javascript: 导航 = %d（起点 URL=%s）", sent, before);
    Sleep(700);

    char after[1024] = {0};
    frame_url_of(frame, after, sizeof(after));
    if (strstr(after, "ncmjs1")) {
        ncm_trace("✅✅ load_url(javascript:) 可用！URL=%s", after);
        frame_call_str_u(frame, 16, "javascript:location.hash='';void(0)");
        g_injections++;
        g_exec_channel_ok = 1;
    } else {
        ncm_trace("❌ load_url(javascript:) 也无效（URL=%s）", after);
    }
}

static int frame_execute_js(void *frame, int idx, const char *code)
{
    return frame_execute_js_u(frame, idx, code, NULL);
}


/* ══════════════════════════════════════════════════════════════════════════
 * 渲染进程终局尝试：调用 v8context 的「返回指针」方法，找真正的 cef_frame_t
 *
 * 之前只读过 ctx 的**字段**（当成 frame 指针用），全部失败；但从没**调用**过
 * ctx 自己的方法。实测 ctx 的 8 个槽里有 4 个返回对象指针（idx 0/2/3/4），
 * 其中一个就该是 get_frame。判据仍然是行为：返回对象上的 idx14 能给出像 URL 的串。
 * 找到 frame 之后再用 location.hash 做闭环自证 execute_java_script 槽位（idx13）——
 * 不依赖任何日志或外部观察。
 * ══════════════════════════════════════════════════════════════════════════ */
/* ══════════════════════════════════════════════════════════════════════════
 * 渲染进程页面注入（实测定位，2026-10-23）
 *
 * 关键实测数据（NCM 3.1.42 / CEF 91.2.3）：
 *   cef_v8context_t  idx3  = get_frame          → 返回 size=0xF0 的对象
 *   该 frame         idx19 = get_url            → 返回 orpheus://orpheus/pub/app.html
 *   该 frame         idx18 = execute_java_script（get_url 前一个槽）
 * 真实槽位比"按头文件推算"的（14 / 13）**整体偏后 5**，这就是之前所有失败的原因。
 * ══════════════════════════════════════════════════════════════════════════ */
/** 进入/退出上下文（实测槽位：enter=5, exit=6）。返回 1 表示成功进入。 */
static int ctx_enter(void *ctx)
{
    uintptr_t fn = 0;
    __try { fn = *(uintptr_t *)((uint8_t *)ctx + NCM_CEF_BASE_SIZE + (uintptr_t)5 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!ncm_ptr_is_code(fn)) return 0;
    int r = 0;
    __try { r = ((int (*)(void *))fn)(ctx); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}

static void ctx_exit(void *ctx)
{
    uintptr_t fn = 0;
    __try { fn = *(uintptr_t *)((uint8_t *)ctx + NCM_CEF_BASE_SIZE + (uintptr_t)6 * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!ncm_ptr_is_code(fn)) return;
    __try { ((void (*)(void *))fn)(ctx); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}
/** 调 v8value 的 set_value_bykey（按给定槽位）。成功返回 1。 */
static int v8_set_key(void *obj, int idx, const char *key, void *val)
{
    wchar_t w[160];
    if (MultiByteToWideChar(CP_UTF8, 0, key, -1, w, 160) <= 1) return 0;
    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = wcslen(w);
    cs.dtor = NULL;
    uintptr_t fn = 0;
    __try { fn = *(uintptr_t *)((uint8_t *)obj + NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!ncm_ptr_is_code(fn)) return 0;
    int r = 0;
    __try { r = ((int (*)(void *, const cef_string_t *, void *, int))fn)(obj, &cs, val, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}

/** 定位 set_value_bykey：写一个新 key，若 has_value_bykey 立刻变 1，就是它。
 *  每个候选用一个不同的 key，避免互相干扰。 */
static int find_set_key_idx(void *global, int has_idx)
{
    static int cached = -2;
    if (cached != -2) return cached;

    void *val = ncm_create_js_string("1");
    if (!val) return cached = -1;

    for (int i = 20; i <= 44; i++) {
        if (i == has_idx) continue;
        char key[64];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "__ncm_sk_%d", i);
        if (!v8_set_key(global, i, key, val)) continue;
        if (v8_has_key(global, has_idx, key) == 1) {
            ncm_trace("  set_value_bykey = idx%d", i);
            return cached = i;
        }
    }
    return cached = -1;
}
/** 调 v8value 的 has_value_bykey（按给定槽位），key 用 UTF-8 传入。 */
static int v8_has_key(void *obj, int idx, const char *key)
{
    wchar_t w[160];
    if (MultiByteToWideChar(CP_UTF8, 0, key, -1, w, 160) <= 1) return -1;
    cef_string_t cs;
    cs.str = (uint16_t *)w;
    cs.length = wcslen(w);
    cs.dtor = NULL;
    uintptr_t fn = 0;
    __try { fn = *(uintptr_t *)((uint8_t *)obj + NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!ncm_ptr_is_code(fn)) return -1;
    int r = -1;
    __try { r = ((int (*)(void *, const cef_string_t *))fn)(obj, &cs); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}


/** 从一个"像 frame"的指针反查 get_url 槽位；成功返回 1 并填 url。 */
static int frame_probe_url(void *frame, char *url, int url_size)
{
    if (!frame) return 0;
    size_t sz = 0;
    __try { sz = *(size_t *)frame; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (sz < 0x40 || sz > 0x400) return 0;

    /* ★ frame 的 get_url 也不整表扫描：CEF 91 的 cef_frame_t 里
     *   undo/redo/cut/copy/paste/del/select_all/view_source 都是「一调就有副作用」的，
     *   load_url 更会直接导航。只试 14=get_url 15=get_browser 16=get_v8context
     *   （都是纯查询、无副作用；后两个返回对象，URL 校验自然会把它们筛掉）。 */
    static const int fcands[] = { 14, 15, 16 };
    for (int fi = 0; fi < 3; fi++) {
        int idx = fcands[fi];
        uintptr_t fn = 0;
        __try { fn = *(uintptr_t *)((uint8_t *)frame + NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!ncm_ptr_is_code(fn)) continue;
        if (try_call_get_url(frame, fn, url, url_size) == 1 && looks_like_url(url)) {
            g_frame_base_off   = NCM_CEF_BASE_SIZE;
            g_frame_url_off    = NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8;
            g_frame_execjs_off = 0;      /* 后面单独行为验证 */
            ncm_trace("browser ✅ frame 的 get_url = idx%d（URL=%s）", idx, url);
            return 1;
        }
    }
    return 0;
}

/** 用 browser 的方法表反查 get_main_frame：谁返回一个"能给出 URL"的对象，谁就是它。 */
static void *browser_get_main_frame(void *browser)
{
    if (g_browser_frame_idx >= 0) {
        void *fn = ((void **)((uint8_t *)browser + NCM_CEF_BASE_SIZE))[g_browser_frame_idx];
        void *f = NULL;
        __try { f = ((void *(*)(void *))fn)(browser); }
        __except (EXCEPTION_EXECUTE_HANDLER) { f = NULL; }
        return f;
    }

    /* ★ 绝不整表扫描：CEF 91 的 cef_browser_t 里 go_back / go_forward / reload /
     *   reload_ignore_cache / stop_load / is_same_instance / get_frame_byident /
     *   get_frame 都是「一调就有副作用，或者必须吃合法参数」的方法，
     *   拿垃圾参数去喊会把整个进程带走（上一版就是这么死的）。
     *   按头文件顺序只试两个纯 getter：12 = get_main_frame，13 = get_focused_frame。 */
    static const int bcands[] = { 12, 13 };
    char url[1024];
    for (int bi = 0; bi < 2; bi++) {
        int idx = bcands[bi];
        uintptr_t fn = 0;
        __try { fn = *(uintptr_t *)((uint8_t *)browser + NCM_CEF_BASE_SIZE + (uintptr_t)idx * 8); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!ncm_ptr_is_code(fn)) continue;

        void *f = NULL;
        __try { f = ((void *(*)(void *))fn)(browser); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        if (!f) { ncm_trace("browser idx%d 返回 NULL（可能是 idx 不对或页面还没建）", idx); continue; }
        if (frame_probe_url(f, url, sizeof(url))) {
            g_browser_frame_idx = idx;
            ncm_trace("browser ✅ get_main_frame = idx%d（frame 可用，URL=%s）", idx, url);
            return f;
        }
        ncm_trace("browser idx%d 返回 %p，但不是可用 frame", idx, f);

        /* 一次性取证：把这个对象的方法指针原样导出（**不调用**），
         * 回 IDA 确认谁是 get_url / execute_java_script，别再猜。 */
        if (!g_frame_dumped) {
            g_frame_dumped = 1;
            uintptr_t cefb = 0; size_t cefsz = 0;
            ncm_module_nolock(L"libcef.dll", &cefb, &cefsz);
            ncm_trace("frame 候选对象 = %p  libcef_base = %p", f, (void *)cefb);
            for (int k = 0; k < 22; k++) {
                void *pv = NULL;
                __try { pv = ((void **)((uint8_t *)f + NCM_CEF_BASE_SIZE))[k]; }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                ncm_trace("  frame[%2d] +0x%02X = %p", k,
                          (unsigned)(NCM_CEF_BASE_SIZE + (uintptr_t)k * 8), pv);
            }
        }
    }
    ncm_trace("browser ❌ 候选 12/13 都没拿到可用 frame");
    return NULL;
}

/** 对 frame 注入，并用 location.hash 闭环自证。 */
static void browser_try_inject(void *browser)
{
    if (g_frame_inject_done) return;

    static int calib_done = 0;
    /* 纯只读校准：get_host(1) 应非空；get_frame_count(16) 告诉我们有没有 frame。
     * 两个调用都无副作用，可以放心反复跑，用来判断 get_main_frame 返回 NULL 的原因。 */
    {
        void *host = NULL; long long fcount = -1;
        __try {
            void *hf = ((void **)((uint8_t *)browser + NCM_CEF_BASE_SIZE))[1];
            if (hf) host = ((void *(*)(void *))hf)(browser);
        } __except (EXCEPTION_EXECUTE_HANDLER) { host = NULL; }
        __try {
            void *cf = ((void **)((uint8_t *)browser + NCM_CEF_BASE_SIZE))[16];
            if (cf) fcount = (long long)((size_t (*)(void *))cf)(browser);
        } __except (EXCEPTION_EXECUTE_HANDLER) { fcount = -2; }
        /* 再校准一层：base.size 决定方法个数；idx0/2/6/10 都是无副作用的纯查询。
         * 若 idx0(is_valid) 不返回 1，说明整张表的起始偏移就不对。 */
        size_t bsz = 0;
        __try { bsz = *(size_t *)browser; } __except (EXCEPTION_EXECUTE_HANDLER) { bsz = 0; }
        int v0 = -9, v2 = -9, v6 = -9; long long v10 = -9;
        __try { void *f = ((void **)((uint8_t *)browser + NCM_CEF_BASE_SIZE))[0];
                if (f) v0 = ((int (*)(void *))f)(browser); } __except (EXCEPTION_EXECUTE_HANDLER) { v0 = -2; }
        __try { void *f = ((void **)((uint8_t *)browser + NCM_CEF_BASE_SIZE))[2];
                if (f) v2 = ((int (*)(void *))f)(browser); } __except (EXCEPTION_EXECUTE_HANDLER) { v2 = -2; }
        __try { void *f = ((void **)((uint8_t *)browser + NCM_CEF_BASE_SIZE))[6];
                if (f) v6 = ((int (*)(void *))f)(browser); } __except (EXCEPTION_EXECUTE_HANDLER) { v6 = -2; }
        __try { void *f = ((void **)((uint8_t *)browser + NCM_CEF_BASE_SIZE))[10];
                if (f) v10 = ((long long (*)(void *))f)(browser); } __except (EXCEPTION_EXECUTE_HANDLER) { v10 = -2; }

        ncm_trace("browser 校准：base.size=0x%zX host=%p frames=%lld | is_valid=%d can_go_back=%d is_loading=%d ident=%lld",
                  bsz, host, fcount, v0, v2, v6, v10);
    }

    void *frame = browser_get_main_frame(browser);
    if (!frame) return;

    char url[1024] = {0};
    if (g_frame_url_off) {
        uintptr_t fn = *(uintptr_t *)((uint8_t *)frame + g_frame_url_off);
        try_call_get_url(frame, fn, url, sizeof(url));
    }
    ncm_trace("browser 当前页面 URL = %s", url);

    /* 1) 先自证 execute_java_script 槽位：改 hash → 再读 URL */
    int url_idx = (int)((g_frame_url_off - NCM_CEF_BASE_SIZE) / 8);
    int cands[4];
    cands[0] = url_idx - 1; cands[1] = url_idx + 1;
    cands[2] = url_idx - 2; cands[3] = url_idx + 2;

    char url2[1024];
    int ok_idx = -1;
    for (int i = 0; i < 4; i++) {
        int ci = cands[i];
        if (ci < 0 || ci > 40) continue;
        if (!frame_execute_js(frame, ci, "try{location.hash='ncmprobe'}catch(e){}")) continue;
        Sleep(350);
        url2[0] = 0;
        uintptr_t fn = *(uintptr_t *)((uint8_t *)frame + g_frame_url_off);
        if (try_call_get_url(frame, fn, url2, sizeof(url2)) == 1 &&
            strstr(url2, "ncmprobe") != NULL) {
            ok_idx = ci;
            ncm_trace("✅✅ execute_java_script = idx%d（hash 已生效，URL 变成 %s）", ci, url2);
            break;
        }
    }

    if (ok_idx < 0) {
        ncm_trace("❌ 4 个候选槽位都不改变 URL —— 浏览器进程执行 JS 未成功");
        return;
    }

    g_frame_execjs_off = NCM_CEF_BASE_SIZE + (uintptr_t)ok_idx * 8;

    /* 2) 复位 hash，别把用户的页面搞脏 */
    frame_execute_js(frame, ok_idx, "try{location.hash=''}catch(e){}");
    Sleep(120);

    /* 3) 正式注入载荷 */
    char *bundle = ncm_bundle_build();
    if (!bundle) return;
    if (strncmp(url, "orpheus://", 10) == 0) {
        if (frame_execute_js(frame, ok_idx, bundle)) {
            g_frame_inject_done = 1;
            g_injections++;
            ncm_trace("✅ 页面载荷已注入（%zu 字节）URL=%s", strlen(bundle), url);
        }
    } else {
        ncm_trace("非 orpheus 页面，先不注入：%s", url);
    }
    free(bundle);
}

/** ★ 返回类型必须是 void*（cef_browser_t*）。
 *  早先写成 int 是致命 bug：x64 下指针被截断，调用方拿到残缺指针必崩 ——
 *  实测表现就是「一注入整个应用就消失」。 */
/* ══════════════════════════════════════════════════════════════════════════
 * client / request_handler 结构 dump（一次性取证）
 *
 * 目的：确认这版客户端是不是走 CEF 的「client → request_handler →
 * get_resource_handler」路径来提供 orpheus:// 页面。
 * 以往 scheme factory 的 create 恒为 0（§7.21），如果这里也拿不到 handler，
 * 就说明页面完全绕开了 CEF 的资源接口，只能去逆向 .ntpk 的解密点。
 *
 * 只做「读指针 + 调 is_valid/get_request_handler 两个安全 getter」，
 * 不遍历、不写、不 patch，风险最低。
 * ══════════════════════════════════════════════════════════════════════════ */
#ifndef NCM_ENABLE_CLIENT_DUMP
#define NCM_ENABLE_CLIENT_DUMP 1
#endif

static int g_client_dumped = 0;

#define CLIENT_GET_REQUEST_HANDLER_OFF (NCM_CEF_BASE_SIZE + 15u * 8u)   /* 0xA0 */

static void dump_itable(const char *tag, void *obj, int max_fields)
{
    if (!obj) { ncm_trace("%s = NULL", tag); return; }
    size_t sz = 0;
    __try { sz = *(size_t *)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { sz = 0; }
    ncm_trace("%s = %p  base.size=0x%zX", tag, obj, sz);
    for (int i = 0; i < max_fields; i++) {
        void *p = NULL;
        __try { p = ((void **)((uint8_t *)obj + NCM_CEF_BASE_SIZE))[i]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        char d[160];
        ncm_describe_addr((uintptr_t)p, d, sizeof(d));
        ncm_trace("  %s[%2d] +0x%02X = %s", tag, i,
                  (unsigned)(NCM_CEF_BASE_SIZE + (uintptr_t)i * 8), d);
    }
}

static void dump_client_once(void *client)
{
    if (g_client_dumped || !client) return;
    g_client_dumped = 1;

    dump_itable("client", client, 16);

    /* get_request_handler 是 cef_client_t 的最后一个成员 */
    uintptr_t grh = 0;
    __try { grh = *(uintptr_t *)((uint8_t *)client + CLIENT_GET_REQUEST_HANDLER_OFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { grh = 0; }
    if (!ncm_ptr_is_code(grh)) {
        ncm_trace("client->get_request_handler(+0xA0) 不是代码指针（%p）", (void *)grh);
        return;
    }

    void *rh = NULL;
    __try { rh = ((void *(*)(void *))grh)(client); }
    __except (EXCEPTION_EXECUTE_HANDLER) { rh = NULL; }
    ncm_trace("client->get_request_handler() → request_handler = %p", rh);
    if (!rh) {
        ncm_trace("→ 宿主没有提供 request_handler：CEF 资源接口这条路不通");
        return;
    }
    dump_itable("reqh", rh, 20);
}
static void dump_client_once(void *client);

static void *hook_create_browser_sync(const void *wi, const void *client, const void *url,
                                      const void *settings, const void *extra, const void *rctx)
{
    LONG n = InterlockedIncrement(&g_create_calls);
    if (n <= 2) ncm_trace("hook: create_browser_sync 第 %ld 次", n);

    /* 一次性取证：client 及其 request_handler 的整张方法表 */
    if (NCM_ENABLE_CLIENT_DUMP) dump_client_once((void *)client);

    void *browser = ((ncm_create_browser_sync_fn)g_orig_create_sync)(wi, client, url, settings,
                                                                    extra, rctx);
    if (NCM_ENABLE_BROWSER_INJECT && browser) {
        g_browser = browser;      /* 此刻 get_main_frame 还是 NULL（实测），留给心跳线程轮询 */
        if (n <= 2) ncm_trace("create_browser_sync 返回 browser=%p（frame 稍后就绪）", browser);

        /* 一次性取证：browser 的方法指针（只读，不调用）。
         * 下一轮拿去 IDA 对 RVA，确认 get_main_frame 到底是哪个槽位。 */
        if (!g_browser_dumped) {
            g_browser_dumped = 1;
            uintptr_t cefb = 0; size_t cefsz = 0;
            ncm_module_nolock(L"libcef.dll", &cefb, &cefsz);
            ncm_trace("browser 对象 = %p  libcef_base = %p", browser, (void *)cefb);
            {
                char od[160];
                ncm_describe_addr((uintptr_t)g_orig_create_sync, od, sizeof(od));
                ncm_trace("原 create_browser_sync 指向 = %s", od);
                for (int q = 0; q < 5; q++) {
                    void *qv = NULL;
                    __try { qv = ((void **)((uint8_t *)browser))[q]; }
                    __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                    ncm_trace("  raw[%d] +0x%02X = %p", q, q * 8, qv);
                }
            }
            for (int k = 0; k < 22; k++) {
                void *pv = NULL;
                __try { pv = ((void **)((uint8_t *)browser + NCM_CEF_BASE_SIZE))[k]; }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                ncm_trace("  browser[%2d] +0x%02X = %p", k,
                          (unsigned)(NCM_CEF_BASE_SIZE + (uintptr_t)k * 8), pv);
            }
        }
    }
    return browser;
}

static int hook_create_browser(const void *wi, const void *client, const void *url,
                               const void *settings, const void *extra, const void *rctx)
{
    LONG n = InterlockedIncrement(&g_create_calls);
    if (n <= 2) ncm_trace("hook: create_browser(async) 第 %ld 次", n);
    return ((int (*)(const void *, const void *, const void *, const void *,
                     const void *, const void *))g_orig_create_async)(wi, client, url, settings, extra, rctx);
}


/** 心跳线程每 2 s 调一次：frame 就绪后才动手（实测 create 时刻 get_main_frame 为 NULL）。 */
/** 插件目录变更后调用：复位页面注入状态，下一轮重新投递新载荷（热重载的生效侧）。 */
void ncm_inject_page_reset(void)
{
    g_injections = 0;
    g_frame_inject_done = 0;
    g_ps.stage = 0;
    g_ps.wait = 0;
    g_ps.cand_i = 0;
    ncm_trace("页面注入状态已复位（热重载：下一轮重新投递）");
}

int ncm_inject_page_try(void)
{
    static int tries = 0;
    if (g_frame_inject_done || !g_browser) return 0;
    if (tries >= 6) return 0;              /* 只试 6 次（≈12 s），之后停手 */
    tries++;
    ncm_trace("browser 注入尝试 %d/6", tries);
    browser_try_inject(g_browser);
    return g_frame_inject_done;
}

int ncm_inject_has_browser(void) { return g_browser != NULL; }
static ncm_patch g_patches[] = {
    { "cef_execute_process",               (void *)hook_execute_process,
      &g_orig_exec,    &g_slot_exec, "CEF 入口，拿 app" },
    { "cef_initialize",                    (void *)hook_initialize,
      &g_orig_init,    &g_slot_init, "浏览器进程入口，拿 app" },
    { "cef_register_extension",            (void *)hook_register_extension,
      &g_orig_reg_ext, NULL,         "页面级 JS 注入载体（宿主自己就用它塞 JS）" },
    { "cef_v8context_get_current_context", (void *)hook_cur_ctx,
      &g_orig_cur_ctx, NULL,         "渲染进程拿 JS 上下文" },
    { "cef_browser_host_create_browser_sync", (void *)hook_create_browser_sync,
      &g_orig_create_sync, NULL,     "浏览器进程拿 cef_browser_t（同步版）" },
#if NCM_ENABLE_MITM_PATCH
    { "cef_register_scheme_handler_factory", (void *)ncm_mitm_register_hook,
      &g_orig_scheme_factory, NULL,  "资源 MITM：改写 orpheus:// 的 HTML（参照 BetterNCM）" },
#endif
    { "cef_browser_host_create_browser",   (void *)hook_create_browser,
      &g_orig_create_async, NULL,    "浏览器进程拿 cef_browser_t（异步版，仅计数）" },
};
#define NCM_PATCH_N ((int)(sizeof(g_patches) / sizeof(g_patches[0])))

/** 装一轮。无锁、无日志、无 CRT、无 psapi —— DllMain 里也能跑。
 *  判据：槽位里的值必须落在 libcef 区间内（＝加载器已经回填真身），
 *  绝不在槽位还是 0/未解析时替换，否则转发会跳到 0。 */
static int ncm_patch_all(void)
{
    uintptr_t cm_base = 0, cef_base = 0;
    size_t    cm_size = 0, cef_size = 0;

    if (!ncm_module_nolock(L"cloudmusic.dll", &cm_base, &cm_size)) return 0;
    if (!ncm_module_nolock(L"libcef.dll",     &cef_base, &cef_size)) return 0;

    int armed = 0;
    for (int i = 0; i < NCM_PATCH_N; i++) {
        ncm_patch *p = &g_patches[i];
        if (*p->orig) continue;                       /* 已装 */

        void **s = ncm_iat_find_slot_in((uint8_t *)cm_base, "libcef.dll", p->fn);
        if (!s) continue;

        uintptr_t cur = (uintptr_t)*s;
        if (cur < cef_base || cur >= cef_base + cef_size) continue;  /* 还没回填 */

        if (!ncm_iat_write_slot(s, p->hook, NULL)) continue;
        *p->orig = (void *)cur;
        if (p->slot) *p->slot = (void *)s;
        armed++;
    }
    return armed;
}

static int all_patched(void)
{
    for (int i = 0; i < NCM_PATCH_N; i++)
        if (!*g_patches[i].orig) return 0;
    return 1;
}

int ncm_patch_count(void)
{
    int n = 0;
    for (int i = 0; i < NCM_PATCH_N; i++) if (*g_patches[i].orig) n++;
    return n;
}

const char *ncm_patch_name(int i)
{
    return (i >= 0 && i < NCM_PATCH_N) ? g_patches[i].fn : "?";
}
int ncm_patch_installed(int i)
{
    return (i >= 0 && i < NCM_PATCH_N) ? (*g_patches[i].orig != NULL) : 0;
}
const char *ncm_patch_why(int i)
{
    return (i >= 0 && i < NCM_PATCH_N) ? g_patches[i].why : "";
}
int ncm_patch_total(void) { return NCM_PATCH_N; }

int ncm_inject_arm_in_dllmain(void)
{
    build_our_rph();   /* 只 memset + 20 次赋值，不用 CRT */

    /* 此刻加载器已经按描述符顺序回填完 libcef(#12) 那一批 IAT 槽位，
     * 而 MSIMG32(#33) 的 DllMain 才刚跑到 —— 零竞争窗口。 */
    int armed = ncm_patch_all();

    if (armed) g_installed = 1;
    g_dllmain_armed = armed;
    return armed;
}

int ncm_inject_dllmain_armed(void) { return g_dllmain_armed; }

/* ══════════════════════════════════════════════════════════════════════════
 * 抢时间：在 cef_execute_process 被调用之前把 IAT 换掉
 * ══════════════════════════════════════════════════════════════════════════ */

static int g_armed_ms = -1;
static int g_verify_fixes = 0;

int ncm_inject_arm_early(int budget_ms)
{
    ULONGLONG t0 = GetTickCount64();

    /* ★ 必须先把自建 handler 填好：
     *   一旦 app 换好，CEF 随时可能来取 handler。如果槽位还全是 NULL，
     *   返回过去就是个空壳，CEF 一按就崩。build_our_rph 只做 memset + 20 次赋值。 */
    build_our_rph();

    int armed = 0;
    while ((int)(GetTickCount64() - t0) < budget_ms) {
        armed += ncm_patch_all();
        /* ★ 判"全部装好"用 orig 有没有值，不是 armed。
         *   如果 DllMain 已经装好了，orig 已经有值，这里必须**立刻退出**；
         *   写成 `if (armed && ...)` 会在 DllMain 成功时空转整个 budget
         *   （实测白等 8 秒，日志因此晚 8 秒才建，见 docs/07 §7.2.3）。 */
        if (all_patched()) break;
        Sleep(1);
    }

    g_armed_ms = (int)(GetTickCount64() - t0);
    if (armed) g_installed = 1;
    return armed;
}

/** 防加载器后写：装好之后再盯一小会儿，槽位被改回去就补回来。 */
int ncm_inject_verify(int budget_ms)
{
    ULONGLONG t0 = GetTickCount64();
    while ((int)(GetTickCount64() - t0) < budget_ms) {
        if (g_slot_exec && *(void **)g_slot_exec != (void *)hook_execute_process) {
            void *prev = NULL;
            if (ncm_iat_write_slot((void **)g_slot_exec, (void *)hook_execute_process, &prev)) {
                g_verify_fixes++;
                ncm_log("⚠️ cef_execute_process 槽位被改回 %p，已重新打补丁", prev);
            }
        }
        if (g_slot_init && *(void **)g_slot_init != (void *)hook_initialize) {
            void *prev = NULL;
            if (ncm_iat_write_slot((void **)g_slot_init, (void *)hook_initialize, &prev)) {
                g_verify_fixes++;
                ncm_log("⚠️ cef_initialize 槽位被改回 %p，已重新打补丁", prev);
            }
        }
        Sleep(1);
    }
    return g_verify_fixes;
}

int ncm_inject_armed_ms(void)  { return g_armed_ms; }
int ncm_inject_verify_fixes(void) { return g_verify_fixes; }

int ncm_inject_install(void)
{
    if (g_installed) return ncm_patch_count();

    build_our_rph();
    ncm_mods_refresh();

    int armed = ncm_patch_all();
    if (!armed && !ncm_patch_count()) {
        ncm_log("install ❌ 一个槽位都没装上");
        return 0;
    }
    g_installed = 1;
    for (int i = 0; i < NCM_PATCH_N; i++)
        ncm_log("install: %-36s %s  （%s）", ncm_patch_name(i),
                ncm_patch_installed(i) ? "✅ 已补" : "❌ 没补", ncm_patch_why(i));
    return ncm_patch_count();
}

int ncm_inject_installed(void)          { return g_installed; }
int ncm_inject_hook_count(void)         { return ncm_patch_count(); }
int ncm_inject_load_handler_calls(void) { return g_slot_calls; }
int ncm_inject_forwarded_calls(void)    { return g_fwd_calls; }
int ncm_inject_injections(void)         { return g_injections; }

void ncm_inject_dump_layout(void)
{
    ncm_log("── 运行时实测布局 ──────────────────────────────");
    ncm_log("  抢装耗时 = %d ms   补写次数 = %d", g_armed_ms, g_verify_fixes);
    ncm_log("  app     base.size = 0x%zX（期望 0x%X）", g_app_size, NCM_APP_EXPECTED_SIZE);
    ncm_log("  app     get_render_process_handler 偏移 = 0x%llX", (unsigned long long)g_rph_off);
    ncm_log("  app     已替换 = %s", g_app_patched ? "是" : "否");
    ncm_log("  rph     原 handler = %p（size 0x%zX）", g_orig_rph,
            g_orig_rph ? *(size_t *)g_orig_rph : 0);
    ncm_log("  rph     on_context_created 实测槽位 = %d", g_ctx_slot);
    ncm_log("  rph     CEF 向我们要 handler 的次数 = %ld", g_get_rph_calls);
    ncm_log("  frame   数组基址偏移 = 0x%llX", (unsigned long long)g_frame_base_off);
    ncm_log("  frame   get_url 偏移 = 0x%llX", (unsigned long long)g_frame_url_off);
    ncm_log("  frame   execute_java_script 偏移 = 0x%llX", (unsigned long long)g_frame_execjs_off);
    ncm_log("  补丁=%d  槽位调用=%d  转发=%d  注入=%d",
            ncm_patch_count(), g_slot_calls, g_fwd_calls, g_injections);
    for (int i = 0; i < NCM_PATCH_N; i++)
        ncm_log("  patch   %-36s %s", ncm_patch_name(i),
                ncm_patch_installed(i) ? "已补" : "未补");
    ncm_log("  MITM      factory_create 调用=%ld   受管 read_response 调用=%ld",
            ncm_mitm_create_calls(), ncm_mitm_read_calls());
    ncm_log("  探针    register_extension 调用=%ld   v8context_get_current 调用=%ld",
            g_reg_ext_calls, g_cur_ctx_calls);
    ncm_log("  原生桥  已注册=%s   JS→DLL 调用=%d（成功 %d）  get_string_value 槽位=%d",
            ncm_host_registered() ? "是" : "否", ncm_host_calls(), ncm_host_ok(),
            ncm_host_string_idx());
}
