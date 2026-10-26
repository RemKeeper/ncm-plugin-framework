/**
 * util.h — 注入层共用的基础设施：日志 / 模块表 / IAT 打补丁 / 字符串转换
 *
 * 所有日志规则遵循 docs/01 §1.6 里实测踩出来的规矩：
 *   - 文件用 _wfsopen(..., _SH_DENYNO)，允许外部边跑边读
 *   - 二进制追加 + 手写 UTF-8 BOM
 *   - 只用窄字符 %s 输出 UTF-8 字节；**绝不用 %ls 输出中文**（会整行截断）
 */
#ifndef NCM_UTIL_H
#define NCM_UTIL_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* ── 日志 ─────────────────────────────────────────────────────────────────── */
void  ncm_log_open(const wchar_t *path);
void  ncm_log_flush(void);
void  ncm_log_close(void);
void  ncm_log(const char *fmt, ...);
void  ncm_log_w(const wchar_t *fmt, ...);   /* 宽字符版本，内部转 UTF-8；标签请用 ASCII */

const wchar_t *ncm_log_path(void);

/** 单个日志文件的大小上限（字节）。默认 8MB，可用环境变量 NCM_LOG_MAX_MB 覆盖（1..256）。 */
long long ncm_log_max_bytes(void);

/** 把已超限的日志轮转为 <path>.1（旧的 .1 会被覆盖）。返回 1 表示已轮转。 */
int ncm_log_rotate(const wchar_t *path);

/** P2 可见效果：给宿主主窗口标题追加标记；成功返回 1。 */
int ncm_badge_apply(const char *suffix);

/* ── 模块表 ───────────────────────────────────────────────────────────────── */
typedef struct ncm_mod {
    wchar_t  name[MAX_PATH];
    wchar_t  path[MAX_PATH];
    uintptr_t base;
    size_t    size;
} ncm_mod;

int       ncm_mods_refresh(void);
int       ncm_mods_count(void);
ncm_mod  *ncm_mods_get(int i);
ncm_mod  *ncm_mods_find(const wchar_t *name);
ncm_mod  *ncm_mods_find_by_addr(uintptr_t addr);

/** addr 是否落在某个已加载模块的代码区间内（可用来判断"这是个函数指针"）。 */
int       ncm_ptr_is_code(uintptr_t addr);

/* ── IAT 打补丁 ───────────────────────────────────────────────────────────── */
/**
 * 把 <mod> 对 <import_mod>!<fn> 的 IAT 槽位换成 new_fn，原来地址写回 *orig。
 *
 * 相比 inline hook 的好处：不需要反汇编、不需要 trampoline、**不依赖 RVA**
 * （CEF 升级后仍然有效）、而且把 *orig 写回就能一键还原。
 *
 * @return 被打补丁的槽位个数（0 = 没找到，说明是 GetProcAddress 动态解析的）
 */
int ncm_iat_hook(const char *mod, const char *import_mod, const char *fn,
                 void *new_fn, void **orig);

/**
 * 极速路径：只定位 / 只写入，不记日志、不分配堆。
 *
 * 为什么需要它：子进程（renderer）在 cloudmusic.dll 加载完之后**几乎立刻**
 * 就调 cef_execute_process，走正常安装流程（开日志、枚举模块、扫插件）要 200ms，
 * 根本抢不到（第一次实机跑就是这个结果，见 docs/07 §7.2.1）。
 *
 * ncm_iat_write_slot 在 `*slot == new_fn` 时返回 0 且**不覆盖** *out_orig ——
 * 这条保证让"反复重试"是安全的（否则会把我们自己的地址当成原地址，转发变成自递归）。
 */
void **ncm_iat_find_slot(const char *mod, const char *import_mod, const char *fn);
int     ncm_iat_write_slot(void **slot, void *new_fn, void **out_orig);

/** 同上，但直接给模块基址 —— 供 DllMain 里用（不走 GetModuleHandleA，不碰加载器锁）。 */
void **ncm_iat_find_slot_in(uint8_t *base, const char *import_mod, const char *fn);

/**
 * 无锁模块查找（PEB 走链，只读内存，不做任何加载器调用）。
 * DllMain 里唯一能安全拿模块基址的办法。
 * @return 1 = 找到
 */
int ncm_module_nolock(const wchar_t *name, uintptr_t *out_base, size_t *out_size);

/** 还原：把槽位写回 orig。 */
int ncm_iat_unhook(void *slot, void *orig);

/* ── 字符串 ───────────────────────────────────────────────────────────────── */
/** cef_string_t(UTF-16) → UTF-8，写进 out（不足则截断）。返回写入长度。 */
int ncm_cefstr_to_utf8(const void *cef_str, char *out, int out_size);

/* ── 地址 → 模块名（用于日志） ───────────────────────────────────────────── */
void ncm_describe_addr(uintptr_t addr, char *out, int out_size);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_UTIL_H */
