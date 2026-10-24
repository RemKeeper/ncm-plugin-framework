/**
 * resdump.c — 资源链取证：hook cloudmusic.dll 的 ResourceLoader::LoadByUrl
 *
 * 目标函数（IDA 确认）：
 *   sub_180D65C20   RVA 0xD65C20
 *   签名（MSVC x64）: char __fastcall(this, const std::wstring* url, std::vector<char>* out)
 *   行为：解析 orpheus:// URL → 磁盘文件 或 ntpk 包 → 填充 out（返回 1 成功）
 *
 * 第一阶段只做**只读转储**：调用原函数，然后把 out 里的字节写到
 *   %LOCALAPPDATA%\NCMPlugin\res\<序号>-<url哈希>.bin
 * 并在 trace 里记录 url → 文件、大小、前若干字节。
 * 这样我们就能拿到解密后的真实 app.html，确认注入点可行后再做改写/重定向。
 */
#include "resdump.h"
#include "ihook.h"
#include "util.h"
#include "plugins.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef NCM_ENABLE_RESDUMP
#define NCM_ENABLE_RESDUMP 1
#endif

/* ── IDA 确认的常量 ─────────────────────────────────────────────────────── */
#define LOADBYURL_RVA   0xD65C20u
/* 序言：3 条 mov（各 5 字节，共 15），无 RIP 相对寻址
 *   4C 89 44 24 08   mov [rsp-8+arg_10], r8
 *   48 89 54 24 08   mov [rsp-8+arg_8],  rdx
 *   48 89 4C 24 08   mov [rsp-8+arg_0],  rcx */
static const unsigned char k_prologue[15] = {
    0x4C, 0x89, 0x44, 0x24, 0x18,   /* mov [rsp+0x18], r8  */
    0x48, 0x89, 0x54, 0x24, 0x10,   /* mov [rsp+0x10], rdx */
    0x48, 0x89, 0x4C, 0x24, 0x08    /* mov [rsp+0x08], rcx */
};

typedef char (__fastcall *load_by_url_fn)(void *self, const void *url_wstring, void *out_vec);

static ncm_ihook *g_hook = NULL;
static volatile LONG g_hits = 0;
static volatile LONG g_injected = 0;
static void *g_op_new = NULL;
static char  *g_bundle = NULL;      /* 页面载荷（__NCM_HOST__ + ncm-page.js） */
static size_t g_bundle_len = 0;   /* 宿主 operator new（RVA 0xC4D714） */

/* ── std::wstring（MSVC）：[0]=ptr/buf, [16]=size, [24]=capacity ────────── */
static int wstring_to_utf8(const void *ws, char *out, int out_size)
{
    out[0] = 0;
    if (!ws) return 0;
    const uintptr_t *w = (const uintptr_t *)ws;
    size_t len = (size_t)w[2];            /* _Mysize @ +16 */
    if (len == 0 || len > 4096) return 0;

    const wchar_t *p;
    if ((size_t)w[3] >= 8) p = (const wchar_t *)w[0];   /* 堆上 */
    else                   p = (const wchar_t *)ws;     /* SSO 内联 */

    int n = WideCharToMultiByte(CP_UTF8, 0, p, (int)len, out, out_size - 1, NULL, NULL);
    if (n <= 0) { out[0] = 0; return 0; }
    out[n] = 0;
    return n;
}

static void sanitize(const char *in, char *out, int out_size)
{
    int j = 0;
    for (int i = 0; in[i] && j < out_size - 1; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') out[j++] = c;
        else if (c == '/' || c == ':' || c == '\\' || c == '?') out[j++] = '_';
    }
    out[j] = 0;
    if (j == 0) strcpy_s(out, (size_t)out_size, "res");
}

static void dump_one(const char *url, const unsigned char *data, size_t len, LONG idx)
{
    /* %LOCALAPPDATA%\NCMPlugin\res */
    char dir[MAX_PATH];
    const char *la = getenv("LOCALAPPDATA");
    if (!la) { ncm_trace("resdump: 没有 LOCALAPPDATA"); return; }
    char sub[MAX_PATH];
    _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\NCMPlugin", la);
    CreateDirectoryA(sub, NULL);
    char sub2[MAX_PATH];
    _snprintf_s(sub2, sizeof(sub2), _TRUNCATE, "%s\\res", sub);
    CreateDirectoryA(sub2, NULL);
    strcpy_s(sub, sizeof(sub), sub2);

    char safe[200];
    sanitize(url, safe, sizeof(safe));
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%03ld-%s.bin", sub, idx, safe);

    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ncm_trace("resdump: 写文件失败 %s err=%lu", path, GetLastError());
        return;
    }
    DWORD wr = 0;
    WriteFile(h, data, (DWORD)len, &wr, NULL);
    CloseHandle(h);

    /* 头部特征，方便一眼看出是不是 HTML/JS */
    char head[33];
    int n = (int)(len < 32 ? len : 32);
    for (int i = 0; i < n; i++) {
        unsigned char c = data[i];
        head[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    head[n] = 0;
    ncm_trace("resdump: #%ld %s → %s（%zu 字节）head=\"%s\"", idx, url, path, len, head);
}

/* ── 我们的替身 ─────────────────────────────────────────────────────────── */
/* ══════════════════════════════════════════════════════════════════════════
 * 页面注入：把脚本追加到 JS chunk 的末尾
 *
 * 原理（docs/07 §7.25）：插件宿主把页面所有 orpheus:// 资源都经过
 * cloudmusic.dll 的 ResourceLoader::LoadByUrl，我们 hook 了它。
 * 于是只要把「原 chunk + 我们的脚本」写回它返回的 std::vector<char>，
 * 页面加载到的 JS 里就自带我们的代码 —— 不需要任何 JS 执行 API。
 *
 * 安全措施：
 *   - 只在 URL 以 .js 结尾时追加；
 *   - 追加时**新分配**缓冲区（HeapAlloc），旧块故意不释放：
 *     vector 的析构会用自己的 operator delete 释放我们给的新指针
 *     （MSVC 里它就是堆分配，能对上），而旧块泄漏一点点，避免跨 CRT 释放崩溃；
 *   - 只有容量校验通过才动手，任何异常都原样透传。
 * ══════════════════════════════════════════════════════════════════════════ */

/* 探针：改 document.title，外部用窗口标题就能验证 DOM 真的被改了 */
static const char k_probe[] =
    /* 只留一个不可见的注入标记（UI 由页面载荷统一渲染成右上角小角标）。
     * 之前这里注入的是顶部整条横幅，收尾时按要求去掉。 */
    "\n;(function(){try{window.__NCM_DOM_PROBE__=1;}catch(e){}})();\n";

/* 给 vector<char> 追加内容。成功返回 1。
 * MSVC std::vector 布局：+0 _Myfirst, +8 _Mylast, +16 _Myend */
/* ══════════════════════════════════════════════════════════════════════════
 * 与 MSVC std::vector<char> 完全兼容的分配（这是不崩的关键）
 *
 * IDA 证据：resize（sub_180079870）在 n >= 0x1000 时并不直接 operator new(n)，
 * 而是 operator new(n + 39)，把结果按 32 字节向上对齐，并在 aligned-8 存原始
 * 指针；析构（sub_1800799D0）会校验：
 *     first - first[-1] - 8  <= 0x1F
 * 不满足就 invalid_parameter_noinfo_noreturn()（直接崩）。
 *
 * 所以我们要复刻同样的分配布局，否则 >4KB 的 chunk 一析构就炸
 * （这正是"追到第 10 个 chunk 进程全灭"的原因）。
 * ══════════════════════════════════════════════════════════════════════════ */
static void *vec_alloc_compatible(size_t n)
{
    if (!g_op_new) return NULL;
    void *(*op_new)(size_t) = (void *(*)(size_t))g_op_new;

    if (n < 0x1000) return op_new(n);

    unsigned char *raw = (unsigned char *)op_new(n + 39);
    if (!raw) return NULL;
    uintptr_t aligned = ((uintptr_t)raw + 39) & ~(uintptr_t)0x1F;
    *(void **)(aligned - 8) = raw;      /* 析构时靠它回溯并 free */
    return (void *)aligned;
}
static int vec_append(void *vec, const char *add, size_t add_len)
{
    if (!vec || !add || add_len == 0) return 0;

    unsigned char *first = *(unsigned char **)((char *)vec + 0);
    unsigned char *last  = *(unsigned char **)((char *)vec + 8);
    unsigned char *end   = *(unsigned char **)((char *)vec + 16);

    size_t len = (size_t)(last - first);
    size_t cap = (size_t)(end - first);
    if (last < first || end < last || len > (64u << 20) || cap > (64u << 20)) {
        ncm_trace("注入: vector 布局不像 vector<char>（len=%zu cap=%zu），跳过", len, cap);
        return 0;
    }

    size_t need = len + add_len + 1;

    /* ★ 复刻 MSVC vector 的分配布局（见 vec_alloc_compatible）：
     *   >=4KB 必须走「new(n+39) + 32 字节对齐 + aligned-8 存原始指针」，
     *   否则析构时的对齐校验会直接 noreturn 崩溃。 */
    unsigned char *nb = (unsigned char *)vec_alloc_compatible(need);
    if (!nb) { ncm_trace("注入: 分配失败"); return 0; }

    if (len) memcpy(nb, first, len);
    memcpy(nb + len, add, add_len);
    nb[len + add_len] = 0;

    /* 旧块故意不 free：交给 vector 析构去释放新块即可，避免跨 CRT 释放 */
    *(unsigned char **)((char *)vec + 0) = nb;
    *(unsigned char **)((char *)vec + 8) = nb + len + add_len;
    *(unsigned char **)((char *)vec + 16) = nb + need;
    return 1;
}

static int url_is_js(const char *url)
{
    size_t n = strlen(url);
    if (n < 3) return 0;
    const char *p = url + n - 3;
    return (p[0] == '.' && (p[1] == 'j' || p[1] == 'J') && (p[2] == 's' || p[2] == 'S'));
}
/* ══════════════════════════════════════════════════════════════════════════
 * 方案 B：磁盘重定向（不碰 LoadByUrl 的缓冲区）
 *
 * 为什么不直接改缓冲区：往 std::vector<char> 里追加会走到跨 CRT 释放，
 * 实测直接把应用带崩（trace 停在 chunk #10，进程全灭）。
 *
 * 改成两步，全部使用宿主自己的分配路径：
 *   1) LoadByUrl 返回后，把「原文 + 我们的载荷」写到
 *        %LOCALAPPDATA%\NCMPlugin\res\patched\<key>.js
 *   2) hook sub_180D67EF0（"这个资源是不是磁盘文件"）：原函数说不是时，
 *      如果我们的 patched 文件存在，就用宿主自己的 wstring 赋值函数
 *      sub_1800B07D0 把路径写进 out，并返回 1。
 *      → LoadByUrl 会走它自己的"读磁盘文件"分支，分配/释放全在它内部完成。
 *   3) 全部准备完后触发一次页面重载，让页面从我们的文件重新加载 JS。
 * ══════════════════════════════════════════════════════════════════════════ */

#define ISFILE_RVA        0x180D67EF0 - 0x180000000u   /* 0xD67EF0 */
#define WSTR_ASSIGN_RVA   0x180B07D0 - 0x180000000u    /* 0xB07D0：wstring 赋值 */

/* sub_180D67EF0 序言：3 条 mov，15 字节（真机实测字节） */
static const unsigned char k_isfile_prologue[15] = {
    0x4C, 0x89, 0x4C, 0x24, 0x20,
    0x4C, 0x89, 0x44, 0x24, 0x18,
    0x48, 0x89, 0x54, 0x24, 0x10
};

typedef char (__fastcall *is_file_fn)(void *self, const void *a2, const void *a3,
                                      const void *a4, char is_skin, void *out_wstring);
typedef void (__fastcall *wstr_assign_fn)(void *dst, const wchar_t *src);

static ncm_ihook *g_isfile_hook = NULL;
static void *g_wstr_assign = NULL;
static volatile LONG g_patched_written = 0;
static volatile LONG g_redirect_hits = 0;

/* patched 目录：%LOCALAPPDATA%\NCMPlugin\res\patched */
static int patched_dir(char *out, size_t out_size)
{
    const char *la = getenv("LOCALAPPDATA");
    if (!la) return 0;
    char base[MAX_PATH];
    _snprintf_s(base, sizeof(base), _TRUNCATE, "%s\\NCMPlugin", la);
    CreateDirectoryA(base, NULL);
    char res[MAX_PATH];
    _snprintf_s(res, sizeof(res), _TRUNCATE, "%s\\res", base);
    CreateDirectoryA(res, NULL);
    char pat[MAX_PATH];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\patched", res);
    CreateDirectoryA(pat, NULL);
    strcpy_s(out, out_size, pat);
    return 1;
}

/* 由 URL 生成稳定的 key（与 is_file 侧用同一套算法） */
static void url_key(const char *url, char *out, int out_size)
{
    /* 去掉 scheme://host 前缀，只保留 path，避免两侧拼法不一致 */
    const char *p = url;
    const char *sep = strstr(url, "://");
    if (sep) {
        p = strchr(sep + 3, '/');
        if (!p) p = sep + 3;
    }
    sanitize(p, out, out_size);
}

/* 把「原文 + 载荷」写成 patched 文件 */
static int write_patched(const char *url, const unsigned char *data, size_t len)
{
    char dir[MAX_PATH];
    if (!patched_dir(dir, sizeof(dir))) return 0;

    char key[200];
    url_key(url, key, sizeof(key));
    if (!key[0]) return 0;

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", dir, key);

    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    DWORD wr = 0;
    WriteFile(h, data, (DWORD)len, &wr, NULL);
    WriteFile(h, k_probe, (DWORD)(sizeof(k_probe) - 1), &wr, NULL);
    CloseHandle(h);
    return 1;
}

/* ── is-file 替身 ────────────────────────────────────────────────────── */
static char __fastcall hook_is_file(void *self, const void *a2, const void *a3,
                                    const void *a4, char is_skin, void *out_wstring)
{
    is_file_fn orig = (is_file_fn)ncm_ihook_tramp(g_isfile_hook);
    if (!orig) return 0;

    char r = orig(self, a2, a3, a4, is_skin, out_wstring);

    /* 诊断：前若干次调用把参数打出来，确认哪个是 path、拼法是否与写入侧一致 */
    static volatile LONG dbg = 0;
    if (InterlockedIncrement(&dbg) <= 24) {
        char s2[400] = "", s3[400] = "", s4[400] = "";
        wstring_to_utf8(a2, s2, sizeof(s2));
        wstring_to_utf8(a3, s3, sizeof(s3));
        wstring_to_utf8(a4, s4, sizeof(s4));
        ncm_trace("is-file: ret=%d skin=%d a2=\"%s\" a3=\"%s\" a4=\"%s\"",
                  (int)r, (int)is_skin, s2, s3, s4);
        if (a3) {
            const uintptr_t *q = (const uintptr_t *)a3;
            ncm_trace("is-file: a3 raw: %p %p %p %p", (void *)q[0], (void *)q[1],
                      (void *)q[2], (void *)q[3]);
        }
    }

    if (r) return 1;          /* 宿主自己就有磁盘文件：不干预 */

    /* 不是磁盘文件 → 看我们有没有对应的 patched 版本 */
    char dir[MAX_PATH];
    if (!patched_dir(dir, sizeof(dir))) return 0;

    const void *cands[3] = { a2, a3, a4 };
    for (int i = 0; i < 3; i++) {
        char s[600];
        if (wstring_to_utf8(cands[i], s, sizeof(s)) <= 0) continue;

        char key[200];
        url_key(s, key, sizeof(key));
        if (!key[0] || !strstr(key, ".js")) continue;

        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", dir, key);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;

        wchar_t wpath[MAX_PATH];
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH) <= 0) continue;

        wstr_assign_fn asg = (wstr_assign_fn)g_wstr_assign;
        if (!asg) return 0;
        __try { asg(out_wstring, wpath); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

        LONG n = InterlockedIncrement(&g_redirect_hits);
        if (n <= 6) ncm_trace("重定向: ✅ %s → %s", s, path);
        return 1;
    }
    return 0;
}

/* 安装 is-file 重定向 hook */
void ncm_resdump_install_redirect(void)
{
    if (g_isfile_hook) return;

    uintptr_t base = 0; size_t size = 0;
    if (!ncm_module_nolock(L"cloudmusic.dll", &base, &size) || !base) return;

    g_wstr_assign = (void *)(base + WSTR_ASSIGN_RVA);
    void *target = (void *)(base + ISFILE_RVA);
    if (!ncm_ihook_install(&g_isfile_hook, target, k_isfile_prologue, 15,
                           (void *)hook_is_file)) {
        ncm_trace("重定向: ❌ is-file hook 安装失败（target=%p）", target);
        return;
    }
    ncm_trace("重定向: ✅ 已接管 is-file 判定（target=%p，wstring assign=%p）",
              target, g_wstr_assign);
}
static char __fastcall hook_load_by_url(void *self, const void *url_wstring, void *out_vec)
{
    load_by_url_fn orig = (load_by_url_fn)ncm_ihook_tramp(g_hook);
    if (!orig) return 0;

    char r = orig(self, url_wstring, out_vec);

    if (r && out_vec) {
        char url[600];
        if (wstring_to_utf8(url_wstring, url, sizeof(url)) > 0) {
            const unsigned char *first = *(const unsigned char **)out_vec;
            const unsigned char *last = *(const unsigned char **)((const char *)out_vec + 8);
            size_t len = (last && first && last > first) ? (size_t)(last - first) : 0;
            if (len > 0 && len < (64u << 20)) {
                LONG idx = InterlockedIncrement(&g_hits);
                dump_one(url, first, len, idx);

                /* ★ 页面注入：探针横幅 + try/catch 包住的完整载荷。
                 *   为什么必须包 try/catch：载荷在页面上下文里可能因为缺少原生桥而抛异常，
                 *   裸追加会把 webpack chunk 的执行带崩（实测整页变白）。 */
                if (url_is_js(url)) {
                    if (!g_bundle) {
                        g_bundle = ncm_bundle_build();
                        g_bundle_len = g_bundle ? strlen(g_bundle) : 0;
                        ncm_trace("注入: 载荷长度 = %zu", g_bundle_len);
                    }
                    int ok = vec_append(out_vec, k_probe, sizeof(k_probe) - 1);
                    if (ok && g_bundle_len) {
                        static const char pre[] = "\n;try{\n";
                        static const char post[] = "\n}catch(e){try{console.error(\"[ncm-inject] payload\",e)}catch(_){}}\n";
                        ok = vec_append(out_vec, pre, sizeof(pre) - 1) &&
                             vec_append(out_vec, g_bundle, g_bundle_len) &&
                             vec_append(out_vec, post, sizeof(post) - 1);
                    }
                    if (ok) {
                        LONG n = InterlockedIncrement(&g_injected);
                        if (n <= 3) ncm_trace("注入: ✅ %s（原 %zu + 载荷 %zu）", url, len, g_bundle_len);
                    } else {
                        ncm_trace("注入: ❌ 失败 %s", url);
                    }
                    write_patched(url, first, len);
                }
            }
        }
    }
    return r;
}

void ncm_resdump_install(void)
{
#if NCM_ENABLE_RESDUMP
    if (g_hook) return;

    uintptr_t base = 0; size_t size = 0;
    if (!ncm_module_nolock(L"cloudmusic.dll", &base, &size) || !base) {
        ncm_trace("resdump: 找不到 cloudmusic.dll");
        return;
    }
    void *target = (void *)(base + LOADBYURL_RVA);
    if (!ncm_ihook_install(&g_hook, target, k_prologue, 15, (void *)hook_load_by_url)) {
        ncm_trace("resdump: ❌ hook 安装失败（target=%p）", target);
        return;
    }
    ncm_trace("resdump: ✅ 已接管 ResourceLoader::LoadByUrl（base=%p target=%p）", (void *)base, target);
    g_op_new = (void *)(base + 0xC4D714u);   /* ??2@YAPEAX_K@Z */
    ncm_resdump_install_redirect();
#endif
}
