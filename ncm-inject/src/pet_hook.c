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
static RECT    g_start_rect;          /* 系统移动循环开始时的窗口位置 */
static DWORD   g_last_click = 0;      /* 去抖：NC/客户端两条路都可能报 click */
static DWORD   g_last_drop = 0;       /* 拖拽刚结束：抑制紧随其后的 NCLBUTTONUP 误报 click */
/* 系统级窗口拖动（HTCAPTION）走的是非客户区消息：
 *   WM_NCLBUTTONDOWN → WM_ENTERSIZEMOVE → WM_MOVING × N → WM_EXITSIZEMOVE
 * 实测宠物窗口的不透明区域返回 HTCAPTION、空白区返回 HTTRANSPARENT，
 * 所以只监听 WM_LBUTTONDOWN 是收不到任何东西的（踩过）。 */
static LRESULT CALLBACK pet_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    /* ── 非客户区（HTCAPTION 拖动）────────────────────────────────── */
    case WM_NCLBUTTONDOWN:
        g_down_x = GET_X_LPARAM(lp);          /* NC 消息是屏幕坐标 */
        g_down_y = GET_Y_LPARAM(lp);
        g_dragging = 0;
        GetWindowRect(h, &g_start_rect);
        ncm_bcast_pet_event("down", g_down_x, g_down_y);
        break;

    case WM_ENTERSIZEMOVE:
        /* 进入系统移动循环：真正开始拖动（等下用 WM_MOVING 判断有没有动） */
        GetWindowRect(h, &g_start_rect);
        g_dragging = 0;
        g_last_drag = 0;
        break;

    case WM_MOVING:
        {
            const RECT *pr = (const RECT *)lp;   /* 建议的新位置（屏幕坐标） */
            if (pr) {
                int dx = pr->left - g_start_rect.left;
                int dy = pr->top  - g_start_rect.top;
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
        }
        break;

    case WM_EXITSIZEMOVE:
        {
            RECT now_rect;
            GetWindowRect(h, &now_rect);
            int dx = now_rect.left - g_start_rect.left;
            int dy = now_rect.top  - g_start_rect.top;
            if (g_dragging) {
                g_last_drop = GetTickCount();
                ncm_bcast_pet_event("drop", dx, dy);
            } else {
                /* 没动 = 点击（系统对 caption 单击也会进出一次移动循环） */
                DWORD t = GetTickCount();
                if (t - g_last_click > 250) {
                    g_last_click = t;
                    ncm_bcast_pet_event("click", g_down_x, g_down_y);
                }
            }
            g_dragging = 0;
        }
        break;

    case WM_NCLBUTTONUP:
        /* 兜底：某些情况下移动循环不会进入。注意拖拽刚结束时系统仍会补一条
         * WM_NCLBUTTONUP，必须抑制，否则每次拖完都会误触发一次点击动画（实测踩到）。 */
        if (!g_dragging) {
            DWORD t = GetTickCount();
            if (t - g_last_click > 250 && t - g_last_drop > 400) {
                g_last_click = t;
                ncm_bcast_pet_event("click", GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
        }
        break;

    /* ── 客户区（万一将来改成手动拖动，也兼容）───────────────────── */
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
            else {
                DWORD t = GetTickCount();
                if (t - g_last_click > 250) { g_last_click = t; ncm_bcast_pet_event("click", x, y); }
            }
            g_dragging = 0;
        }
        break;

    default:
        break;
    }
    return CallWindowProcW(g_orig, h, msg, wp, lp);
}

/* 找窗口并子类化；窗口可能比我们晚创建，也可能被重建，所以定期检查 */
/* 找窗口并（重新）挂上子类化。
 *
 * ★ 不能只用 IsWindow(g_pet) 判断"还挂着"：网易云会重建桌宠窗口，
 *   窗口句柄值可能被系统复用，于是 IsWindow 通过、而我们其实早已不在
 *   proc 链上（实测：拖拽时窗口在动、但一条事件都收不到）。
 *   正确的判断是 GetWindowLongPtr(GWLP_WNDPROC) 是否就是我们自己。 */
static void try_attach(void)
{
    HWND h = FindWindowW(PET_CLASS, NULL);
    if (!h) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return;   /* 只钩自己进程里的那个 */

    WNDPROC cur = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (cur == pet_proc) { g_pet = h; return; } /* 已经是我们 */

    SetLastError(0);
    WNDPROC orig = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)pet_proc);
    if (!orig) {
        ncm_log("pet: 挂钩失败 hwnd=%p err=%lu", (void *)h, GetLastError());
        return;
    }
    g_orig = orig;
    g_pet  = h;
    ncm_log("pet: %s hwnd=%p（点击/拖拽事件经 SSE 推送）",
            cur ? "🔄 重新挂钩桌宠窗口" : "✅ 已挂钩桌宠窗口", (void *)h);
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