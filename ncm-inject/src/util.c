/**
 * util.c — 注入层基础设施实现
 */
#include "util.h"
#include "cef_capi.h"   /* cef_string_t */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <share.h>      /* _SH_DENYNO */
#include <psapi.h>
#include <intrin.h>     /* __readgsqword */

#pragma comment(lib, "psapi.lib")

/* ══════════════════════════════════════════════════════════════════════════
 * 日志
 * ══════════════════════════════════════════════════════════════════════════ */

static FILE          *g_log = NULL;
static CRITICAL_SECTION g_log_cs;
static int             g_log_cs_ready = 0;
static wchar_t         g_log_path[MAX_PATH] = {0};

/* ── 日志文件建立之前的行缓冲 ────────────────────────────────────────────────
 * 为什么需要：抢装路径（ncm_inject_arm_early）必须跑在任何文件 I/O 之前，
 * 而它换掉的那个入口（子进程里的 cef_execute_process）会在**几毫秒内**就被调用 ——
 * 那时日志文件还没建。第一次实机跑就是这样：hook 明明进去了，日志里却一条没有，
 * 看起来像"没 hook 上"，白排查一轮（docs/07 §7.2.1）。
 * 这些行先存内存，ncm_log_open 时回放到文件里。
 * 用 InterlockedIncrement 占位，无锁、可重入、不需要 CRT 锁 ——
 * 因为此刻可能同时有 CEF 主线程和我们的 worker 两个线程在写。 */
#define EARLY_MAX 96
#define EARLY_LEN 320
static char           g_early[EARLY_MAX][EARLY_LEN];
static volatile LONG  g_early_n = 0;
static volatile LONG  g_early_lost = 0;

void ncm_log_open(const wchar_t *path)
{
    if (!g_log_cs_ready) {
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = 1;
    }
    if (g_log) return;

    wcsncpy_s(g_log_path, MAX_PATH, path, _TRUNCATE);

    /* 目录可能不存在 */
    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, MAX_PATH, path, _TRUNCATE);
    wchar_t *p = wcsrchr(dir, L'\\');
    if (p) { *p = 0; CreateDirectoryW(dir, NULL); }

    /* ★ _wfsopen + _SH_DENYNO：允许外部在进程运行时读取（docs/01 §1.6.2 第 5 条）
     *   _wfopen_s 是独占的，会把观测侧完全挡在门外。 */
    g_log = _wfsopen(path, L"ab", _SH_DENYNO);
    if (!g_log) return;

    /* 新文件（或空文件）写 UTF-8 BOM。不用 "a, ccs=UTF-8"（中文会炸）。 */
    fseek(g_log, 0, SEEK_END);
    if (ftell(g_log) == 0) {
        fwrite("\xEF\xBB\xBF", 1, 3, g_log);
    }

    /* ── 回放抢装阶段的行 ── */
    EnterCriticalSection(&g_log_cs);
    LONG total = g_early_n;
    LONG kept = total > EARLY_MAX ? EARLY_MAX : total;
    LONG lost = total - kept;
    if (kept > 0 || lost > 0) {
        fprintf(g_log, "[--] ── 以下是日志文件建立前产生的 %ld 条（抢装阶段）──\n", kept);
        for (LONG i = 0; i < kept; i++) fprintf(g_log, "%s\n", g_early[i]);
        if (lost > 0) fprintf(g_log, "[--] （另有 %ld 条因缓冲满被丢弃）\n", lost);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_log_cs);

    ncm_log("=== ncm-inject 日志开始 ===");
}

const wchar_t *ncm_log_path(void) { return g_log_path; }

void ncm_log_flush(void)
{
    if (g_log) fflush(g_log);
}

void ncm_log_close(void)
{
    if (g_log) { fflush(g_log); fclose(g_log); g_log = NULL; }
}

void ncm_log(const char *fmt, ...)
{
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    if (g_log_cs_ready) EnterCriticalSection(&g_log_cs);
    if (g_log) {
        fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
        fflush(g_log);
    } else {
        /* 文件还没建：先存内存，ncm_log_open 时回放 */
        char line[EARLY_LEN];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d] %s",
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);
        LONG idx = InterlockedIncrement(&g_early_n) - 1;
        if (idx < EARLY_MAX) {
            strncpy_s(g_early[idx], EARLY_LEN, line, _TRUNCATE);
        } else {
            InterlockedIncrement(&g_early_lost);
        }
        OutputDebugStringA(body);
    }
    if (g_log_cs_ready) LeaveCriticalSection(&g_log_cs);
}

void ncm_log_w(const wchar_t *fmt, ...)
{
    wchar_t wbuf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(wbuf, 2048, _TRUNCATE, fmt, ap);
    va_end(ap);
    /* 宽 → UTF-8，再走窄日志路径 */
    char u8[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, u8, sizeof(u8), NULL, NULL);
    if (n > 0) ncm_log("%s", u8);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 模块表 —— ★ 只允许 worker 线程使用
 *
 * 血泪教训：这个函数原来用 EnterCriticalSection(&g_log_cs) 做保护，
 * 而 g_log_cs 要等 ncm_log_open 才初始化。结果 CEF 回调（主线程）在日志
 * 还没建好时调到这里，等于对一个**未初始化的 CRITICAL_SECTION** 下锁 ——
 * 未定义行为，实机表现为主线程直接卡死、子进程一个都不生（docs/07 §7.2.3）。
 *
 * 结论：CEF 回调路径上不许碰 psapi，也不许碰任何"可能还没初始化"的锁。
 * 需要"这个地址是不是代码"时一律走无锁 PEB 走链（见 ncm_ptr_is_code）。
 * ══════════════════════════════════════════════════════════════════════════ */

static ncm_mod g_mods[512];
static int     g_mod_count = 0;

int ncm_mods_refresh(void)
{
    HMODULE h[512];
    DWORD   needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), h, sizeof(h), &needed)) return 0;

    int n = (int)(needed / sizeof(HMODULE));
    if (n > 512) n = 512;

    /* 单写者（worker），不需要锁 */
    g_mod_count = 0;
    for (int i = 0; i < n; i++) {
        ncm_mod *m = &g_mods[g_mod_count];
        MODULEINFO mi;
        if (!GetModuleInformation(GetCurrentProcess(), h[i], &mi, sizeof(mi))) continue;
        m->base = (uintptr_t)mi.lpBaseOfDll;
        m->size = (size_t)mi.SizeOfImage;
        GetModuleFileNameW(h[i], m->path, MAX_PATH);
        const wchar_t *nm = wcsrchr(m->path, L'\\');
        wcsncpy_s(m->name, MAX_PATH, nm ? nm + 1 : m->path, _TRUNCATE);
        g_mod_count++;
    }
    return g_mod_count;
}

int      ncm_mods_count(void)      { return g_mod_count; }
ncm_mod *ncm_mods_get(int i)       { return (i >= 0 && i < g_mod_count) ? &g_mods[i] : NULL; }

static int name_eq(const wchar_t *a, const wchar_t *b)
{
    return _wcsicmp(a, b) == 0;
}

ncm_mod *ncm_mods_find(const wchar_t *name)
{
    for (int i = 0; i < g_mod_count; i++)
        if (name_eq(g_mods[i].name, name)) return &g_mods[i];
    return NULL;
}

ncm_mod *ncm_mods_find_by_addr(uintptr_t addr)
{
    for (int i = 0; i < g_mod_count; i++)
        if (addr >= g_mods[i].base && addr < g_mods[i].base + g_mods[i].size)
            return &g_mods[i];
    return NULL;
}

/* ★ 这两个函数会在 CEF 回调线程上被调用，所以**不能**用 g_mods 表
 *   （表由 worker 重建，且历史上它的锁还没初始化就被下过锁 → 主线程卡死）。
 *   一律走无锁 PEB 走链。 */
static int ncm_mod_of_addr_nolock(uintptr_t addr, const wchar_t **name, uintptr_t *base);

int ncm_ptr_is_code(uintptr_t addr)
{
    if (addr < 0x10000) return 0;
    return ncm_mod_of_addr_nolock(addr, NULL, NULL);
}

void ncm_describe_addr(uintptr_t addr, char *out, int out_size)
{
    const wchar_t *nm = NULL;
    uintptr_t mbase = 0;
    if (addr >= 0x10000 && ncm_mod_of_addr_nolock(addr, &nm, &mbase) && nm) {
        char u8[MAX_PATH * 3];
        int n = WideCharToMultiByte(CP_UTF8, 0, nm, -1, u8, sizeof(u8), NULL, NULL);
        if (n <= 0) u8[0] = 0;
        _snprintf_s(out, out_size, _TRUNCATE, "%s+0x%llX", u8,
                    (unsigned long long)(addr - mbase));
    } else {
        _snprintf_s(out, out_size, _TRUNCATE, "(不在任何模块内) %p", (void *)addr);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * 无锁模块查找（PEB 走链）
 * ══════════════════════════════════════════════════════════════════════════ */

/* 结构体偏移按 x64 硬写死 —— 只做内存读，不做任何加载器调用，
 * 所以可以在 DllMain 里安全使用（GetModuleHandleA 会去拿加载器锁，
 * 而此刻锁正被我们自己持有）。 */
typedef struct {
    BYTE        _pad[0x18];
    void       *Ldr;                 /* PEB+0x18 */
} NCM_PEB;

typedef struct {
    BYTE        _pad[0x10];
    LIST_ENTRY  InLoadOrderModuleList;  /* PEB_LDR_DATA+0x10 */
} NCM_LDR;

typedef struct {
    LIST_ENTRY     InLoadOrderLinks;    /* +0x00 */
    LIST_ENTRY     InMemoryOrderLinks;  /* +0x10 */
    LIST_ENTRY     InInitOrderLinks;    /* +0x20 */
    void          *DllBase;             /* +0x30 */
    void          *EntryPoint;          /* +0x38 */
    ULONG          SizeOfImage;         /* +0x40 */
    ULONG          _pad;
    struct { USHORT Length, MaxLength; wchar_t *Buffer; } FullDllName;  /* +0x48 */
    struct { USHORT Length, MaxLength; wchar_t *Buffer; } BaseDllName;  /* +0x58 */
} NCM_LDR_ENTRY;

static int ieq_w(const wchar_t *buf, USHORT len_bytes, const wchar_t *name)
{
    if (!buf || !name) return 0;
    size_t n = len_bytes / sizeof(wchar_t);
    size_t i = 0;
    for (; i < n && name[i]; i++) {
        wchar_t a = buf[i], b = name[i];
        if (a >= L'A' && a <= L'Z') a = (wchar_t)(a + 32);
        if (b >= L'A' && b <= L'Z') b = (wchar_t)(b + 32);
        if (a != b) return 0;
    }
    return i == n && name[i] == 0;
}

int ncm_module_nolock(const wchar_t *name, uintptr_t *out_base, size_t *out_size)
{
#ifdef _M_X64
    NCM_PEB *peb = (NCM_PEB *)__readgsqword(0x60);
#else
    NCM_PEB *peb = NULL;
#endif
    if (!peb || !peb->Ldr) return 0;

    NCM_LDR *ldr = (NCM_LDR *)peb->Ldr;
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    LIST_ENTRY *e = head->Flink;

    /* 防御：链表万一异常，最多走 4096 个就停 */
    for (int guard = 0; e && e != head && guard < 4096; guard++, e = e->Flink) {
        NCM_LDR_ENTRY *m = (NCM_LDR_ENTRY *)e;
        if (ieq_w(m->BaseDllName.Buffer, m->BaseDllName.Length, name)) {
            if (out_base) *out_base = (uintptr_t)m->DllBase;
            if (out_size) *out_size = (size_t)m->SizeOfImage;
            return 1;
        }
    }
    return 0;
}

/** addr 落在哪个模块里？返回 1 = 找到。无锁、只读内存、可在 CEF 回调线程用。 */
static int ncm_mod_of_addr_nolock(uintptr_t addr, const wchar_t **name, uintptr_t *base)
{
#ifdef _M_X64
    NCM_PEB *peb = (NCM_PEB *)__readgsqword(0x60);
#else
    NCM_PEB *peb = NULL;
#endif
    if (!peb || !peb->Ldr) return 0;

    NCM_LDR *ldr = (NCM_LDR *)peb->Ldr;
    LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
    LIST_ENTRY *e = head->Flink;

    for (int guard = 0; e && e != head && guard < 4096; guard++, e = e->Flink) {
        NCM_LDR_ENTRY *m = (NCM_LDR_ENTRY *)e;
        uintptr_t b = (uintptr_t)m->DllBase;
        if (addr >= b && addr < b + (uintptr_t)m->SizeOfImage) {
            if (name) *name = m->BaseDllName.Buffer;
            if (base) *base = b;
            return 1;
        }
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * IAT 打补丁
 * ══════════════════════════════════════════════════════════════════════════ */

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/** 只定位，不写。不记日志、不分配 —— 供抢时间用的极速路径调用。 */
void **ncm_iat_find_slot_in(uint8_t *base, const char *import_mod, const char *fn)
{
    if (!base) return NULL;

    IMAGE_DOS_HEADER   *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    IMAGE_NT_HEADERS64 *nt  = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || !dd->Size) return NULL;

    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress);

    for (; desc->Name; desc++) {
        const char *dll = (const char *)(base + desc->Name);
        if (!ieq(dll, import_mod)) continue;

        IMAGE_THUNK_DATA64 *ilt = (IMAGE_THUNK_DATA64 *)(base + (desc->OriginalFirstThunk
                                     ? desc->OriginalFirstThunk : desc->FirstThunk));
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)(base + desc->FirstThunk);

        for (int i = 0; ilt[i].u1.AddressOfData; i++) {
            if (ilt[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
            IMAGE_IMPORT_BY_NAME *ibn =
                (IMAGE_IMPORT_BY_NAME *)(base + ilt[i].u1.AddressOfData);
            if (!ieq((const char *)ibn->Name, fn)) continue;
            return (void **)&iat[i];
        }
    }
    return NULL;
}

void **ncm_iat_find_slot(const char *mod, const char *import_mod, const char *fn)
{
    HMODULE hm = GetModuleHandleA(mod);
    if (!hm) return NULL;
    return ncm_iat_find_slot_in((uint8_t *)hm, import_mod, fn);
}

/**
 * 写一个槽位。
 * 返回：1 = 本次真的改了，*out_orig 是替换前的值
 *       0 = 槽位已经是 new_fn（无需重写），或 VirtualProtect 失败
 * ★ 这个 0 分支很关键：重试逻辑会反复调它，如果每次都把 `*slot`（也就是我们自己的
 *   函数地址）当成"原地址"存进 out_orig，转发就会变成自己调自己 → 无限递归。
 */
int ncm_iat_write_slot(void **slot, void *new_fn, void **out_orig)
{
    if (!slot) return 0;
    if (*slot == new_fn) return 0;

    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old_prot)) return 0;
    void *prev = *slot;
    *slot = new_fn;
    VirtualProtect(slot, sizeof(void *), old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));

    if (out_orig) *out_orig = prev;
    return 1;
}

int ncm_iat_hook(const char *mod, const char *import_mod, const char *fn,
                 void *new_fn, void **orig)
{
    if (orig) *orig = NULL;

    if (!GetModuleHandleA(mod)) { ncm_log("IAT: 模块 %s 未加载", mod); return 0; }

    void **slot = ncm_iat_find_slot(mod, import_mod, fn);
    if (!slot) {
        ncm_log("IAT ❌ 没找到 %s!%s 的 IAT 槽位（可能是 GetProcAddress 动态解析）",
                import_mod, fn);
        return 0;
    }

    void *prev = NULL;
    if (!ncm_iat_write_slot(slot, new_fn, &prev)) {
        ncm_log("IAT: %s!%s 已经是我们的实现，跳过", import_mod, fn);
        return 0;
    }

    if (orig) *orig = prev;

    char d[192];
    ncm_describe_addr((uintptr_t)prev, d, sizeof(d));
    ncm_log("IAT ✅ %s!%s  槽位=%p  原地址=%s → 0x%p",
            import_mod, fn, (void *)slot, d, new_fn);
    return 1;
}

int ncm_iat_unhook(void *slot, void *orig)
{
    if (!slot) return 0;
    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old_prot)) return 0;
    *(void **)slot = orig;
    VirtualProtect(slot, sizeof(void *), old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return 1;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 字符串
 * ══════════════════════════════════════════════════════════════════════════ */

int ncm_cefstr_to_utf8(const void *cef_str, char *out, int out_size)
{
    if (out_size <= 0) return 0;
    out[0] = 0;
    if (!cef_str) return 0;
    const cef_string_t *s = (const cef_string_t *)cef_str;
    if (!s->str || s->length == 0) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)s->str, (int)s->length,
                                out, out_size - 1, NULL, NULL);
    if (n <= 0) return 0;
    out[n] = 0;
    return n;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 宿主窗口标记（P2「可见效果」）
 *
 * 为什么用「改标题」当作可见效果：
 *   - 它是**外部可验证**的：`(Get-Process cloudmusic).MainWindowTitle` 直接读得到，
 *     不需要截图、不需要人眼看，能进自动化证据链；
 *   - 它证明的是「我们的代码在宿主进程里跑起来了，而且能操作宿主的 UI 对象」，
 *     这比弹一个自己的窗口更能说明注入成功；
 *   - 完全可逆（追加后缀，不覆盖原标题），对宿主功能零影响。
 *
 * 只对「本进程自己拥有的、可见的、有标题的」顶层窗口动手，
 * 所以渲染进程 / GPU 进程调用它什么也不会发生（它们没有这样的窗口）。
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct badge_ctx {
    DWORD  pid;
    int    hits;
    wchar_t before[256];
    wchar_t after[256];
} badge_ctx;

static BOOL CALLBACK badge_enum_proc(HWND hwnd, LPARAM lp)
{
    badge_ctx *c = (badge_ctx *)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != c->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;      /* 跳过工具窗/弹窗 */

    int n = GetWindowTextW(hwnd, c->before, 256);
    if (n <= 0) return TRUE;                                  /* 没标题的多半是隐藏窗口 */

    /* 已经打过标记就不重复加 */
    if (wcsstr(c->before, L"[ncm-inject]")) { c->hits++; return FALSE; }

    _snwprintf_s(c->after, 256, _TRUNCATE, L"%s [ncm-inject]", c->before);
    if (SetWindowTextW(hwnd, c->after)) {
        c->hits++;
        ncm_log("★ 可见效果：已改宿主窗口标题 → %s", "");
        ncm_log_w(L"   原标题：%s", c->before);
        ncm_log_w(L"   现标题：%s", c->after);
        return FALSE;
    }
    return TRUE;
}

int ncm_badge_apply(const char *suffix)
{
    (void)suffix;   /* 后缀固定写死，避免调用方传进来奇怪字符 */
    badge_ctx c;
    ZeroMemory(&c, sizeof(c));
    c.pid = GetCurrentProcessId();
    EnumWindows(badge_enum_proc, (LPARAM)&c);
    return c.hits > 0;
}