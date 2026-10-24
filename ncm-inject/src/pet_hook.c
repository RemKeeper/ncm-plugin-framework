/*
 * pet_hook.c —— 桌宠窗口的鼠标交互旁路
 *
 * 背景（实测）：桌宠窗口是 CEF 的 OSR（离屏渲染）窗口，类名
 *   CloudMusicDesktopPetOsrView
 * 宿主只把 WM_MOUSEMOVE 转发进页面，**按键事件（mousedown/up/click）根本到不了页面**——
 * 页面里 window 上挂 mousedown 监听，用真实输入注入后计数始终是 0。
 *
 * 所以点击响应与拖拽只能在这一层做：子类化该窗口的 WndProc，**只观察不消费**
 * （处理完照常 CallWindowProc 交还给网易云，不影响它自己的拖拽/菜单），
 * 再通过 bcast 的 SSE 把事件推给页面插件。
 *
 * 事件序列：
 *   down      左键按下
 *   dragstart 位移超过阈值（第一次）
 *   drag      拖动中（16fps 节流）
 *   drop      拖动后松手
 *   click     没拖动就松手（= 点击）
 */

#include <windows.h>
#include <windowsx.h>
#include <process.h>
#include <stdio.h>

#include "pet_hook.h"
#include "bcast.h"
#include "util.h"

#define PET_CLASS      L"CloudMusicDesktopPetOsrView"
#define DRAG_THRESHOLD 6     /* px，超过才算拖拽而不是手抖 */
#define DRAG_THROTTLE  60    /* ms，拖动事件节流 */

static HWND    g_pet = NULL;
static WNDPROC g_orig = NULL;
static int     g_down_x = 0, g_down_y = 0;
static int     g_dragging = 0;
static DWORD   g_last_drag = 0;

static LRESULT CALLBACK pet_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN:
        g_down_x = GET_X_LPARAM(lp);
        g_down_y = GET_Y_LPARAM(lp);
        g_dragging = 0;
        ncm_bcast_pet_event("down", g_down_x, g_down_y);
        break;

    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON) {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int dx = x - g_down_x, dy = y - g_down_y;
            if (!g_dragging && (dx * dx + dy * dy) > (DRAG_THRESHOLD * DRAG_THRESHOLD)) {
                g_dragging = 1;
                g_last_drag = GetTickCount();
                ncm_bcast_pet_event("dragstart", dx, dy);
            } else if (g_dragging) {
                DWORD now = GetTickCount();
                if (now - g_last_drag >= DRAG_THROTTLE) {
                    g_last_drag = now;
                    ncm_bcast_pet_event("drag", dx, dy);
                }
            }
        }
        break;

    case WM_LBUTTONUP:
        {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (g_dragging) ncm_bcast_pet_event("drop", x - g_down_x, y - g_down_y);
            else            ncm_bcast_pet_event("click", x, y);
            g_dragging = 0;
        }
        break;

    default:
        break;
    }
    return CallWindowProcW(g_orig, h, msg, wp, lp);
}

/* 找窗口并子类化；窗口可能比我们晚创建，也可能被重建，所以定期检查 */
static void try_attach(void)
{
    if (g_pet && IsWindow(g_pet)) return;

    HWND h = FindWindowW(PET_CLASS, NULL);
    if (!h) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return;   /* 只钩自己进程里的那个 */

    SetLastError(0);
    WNDPROC orig = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)pet_proc);
    if (!orig) {
        ncm_log("pet: 挂钩失败 hwnd=%p err=%lu", (void *)h, GetLastError());
        return;
    }
    g_orig = orig;
    g_pet  = h;
    ncm_log("pet: ✅ 已挂钩桌宠窗口 hwnd=%p（点击/拖拽事件经 SSE 推送）", (void *)h);
    ncm_bcast_pet_event("attach", 0, 0);
}

static unsigned __stdcall pet_thread(void *arg)
{
    (void)arg;
    ncm_log("pet: 交互钩子线程启动，等待桌宠窗口…");
    for (;;) {
        try_attach();
        Sleep(1500);
    }
    return 0;
}

void ncm_pet_hook_start(void)
{
    static int started = 0;
    if (InterlockedExchange(&started, 1)) return;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, pet_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
