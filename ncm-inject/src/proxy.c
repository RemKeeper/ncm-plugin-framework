/**
 * proxy.c — MSIMG32 转发槽位
 *
 * proxy_msimg32.asm 里那 5 个过程只有一条 `jmp qword ptr [槽位]`，
 * 槽位就是下面这 5 个全局。真身在 System32\msimg32.dll。
 *
 * ★ 为什么必须在 DllMain 里就接上：
 *   宿主（cloudmusic.dll）是**静态导入** MSIMG32 的，加载器解析导入时就会用到这些导出；
 *   等我们自己的线程起来再接，中间有一段窗口期转发目标还是 NULL → 一跳就崩。
 *
 * ★ 为什么用 LOAD_LIBRARY_SEARCH_SYSTEM32：
 *   直接 LoadLibraryW(L"msimg32.dll") 会走"应用目录优先"，
 *   于是又命中**我们自己**，形成自我转发 → 无限递归 → 栈溢出。
 *   这个坑在 probe 阶段已经踩过并记录（docs/01 §1.6.3）。
 */
#include <windows.h>
#include <stdio.h>

void *g_pfn_vSetDdrawflag  = NULL;
void *g_pfn_AlphaBlend     = NULL;
void *g_pfn_DllInitialize  = NULL;
void *g_pfn_GradientFill   = NULL;
void *g_pfn_TransparentBlt = NULL;

static HMODULE g_real = NULL;

HMODULE ncm_proxy_real_module(void) { return g_real; }

int ncm_proxy_resolve(void)
{
    if (g_real) return 1;

    g_real = LoadLibraryExW(L"C:\\Windows\\System32\\msimg32.dll", NULL,
                            LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_real)
        g_real = LoadLibraryW(L"C:\\Windows\\System32\\msimg32.dll");
    if (!g_real) return 0;

    g_pfn_vSetDdrawflag  = (void *)GetProcAddress(g_real, "vSetDdrawflag");
    g_pfn_AlphaBlend     = (void *)GetProcAddress(g_real, "AlphaBlend");
    g_pfn_DllInitialize  = (void *)GetProcAddress(g_real, "DllInitialize");
    g_pfn_GradientFill   = (void *)GetProcAddress(g_real, "GradientFill");
    g_pfn_TransparentBlt = (void *)GetProcAddress(g_real, "TransparentBlt");

    return (g_pfn_AlphaBlend && g_pfn_GradientFill && g_pfn_TransparentBlt) ? 1 : 0;
}
