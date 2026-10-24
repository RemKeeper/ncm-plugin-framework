/**
 * inject.h — 注入层对外接口
 */
#ifndef NCM_INJECT_H
#define NCM_INJECT_H

#include <windows.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * ★ DllMain 内抢装：唯一的零竞争路径。
 * 必须在 DllMain 里、起线程之前调用。
 * 依据：cloudmusic.dll 的导入描述符里 libcef 是 #12、MSIMG32 是 #33，
 * 加载器按序回填 IAT ⇒ 轮到我们时 libcef 的槽位已经是真身地址。
 * 不开日志、不调加载器、不碰 CRT 堆；结果只记在全局变量里，由 worker 记日志。
 */
int ncm_inject_arm_in_dllmain(void);
int ncm_inject_dllmain_armed(void);

/**
 * 抢时间版安装：必须在 worker 线程的**最开头**调用（任何开日志/枚举模块之前），
 * 否则会输给子进程里紧跟 cloudmusic.dll 加载之后的 cef_execute_process。
 * 不记日志、不分配堆。返回本次抢到的补丁数。
 */
int ncm_inject_arm_early(int budget_ms);

/** 补丁表状态：装了几个 / 每个的名字、是否已装、为什么打（用于日志逐条列出）。 */
int         ncm_patch_count(void);
int         ncm_patch_total(void);
const char *ncm_patch_name(int i);
int         ncm_patch_installed(int i);
const char *ncm_patch_why(int i);

/** 装好之后再盯一小会儿，槽位被加载器改回去就补回来。返回补写次数。 */
int ncm_inject_verify(int budget_ms);

int ncm_inject_armed_ms(void);
int ncm_inject_verify_fixes(void);

/** 尝试安装 CEF 注入（需要 libcef.dll 已加载）。返回已安装的补丁数。 */
int ncm_inject_install(void);

int  ncm_inject_installed(void);
int  ncm_inject_hook_count(void);
int  ncm_inject_load_handler_calls(void);
int  ncm_inject_forwarded_calls(void);
int  ncm_inject_injections(void);

/** 运行时解析出来的偏移，供日志/诊断用。 */
void ncm_inject_dump_layout(void);

/** 浏览器进程：frame 就绪后尝试页面注入（心跳线程调用）。 */
int  ncm_inject_page_try(void);

/** 热重载：复位页面注入状态，让下一轮重新投递载荷。 */
void ncm_inject_page_reset(void);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_INJECT_H */
