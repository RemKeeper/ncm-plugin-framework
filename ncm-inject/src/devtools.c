/**
 * devtools.c — 已废弃的实现（保留记录）
 *
 * 曾尝试 hook kernel32!GetCommandLineW 追加 --remote-debugging-port，
 * 实测两点失败：
 *   1) libcef 在 DLL 加载阶段就已经读过命令行，DllMain 里装钩子也已经晚于它；
 *   2) 该函数是热路径小桩，12 字节 patch 未校验原始字节，导致进程崩溃（实测进程数掉到 1）。
 *
 * 现行做法见 inject.c 的 devtools_patch_settings()：直接改 CefSettings 的字段。
 */
#include "devtools.h"
#include "util.h"

void ncm_devtools_install(void)
{
    /* 空实现：DevTools 由 cef_initialize 钩子里的 CefSettings 补丁完成 */
}
