/**
 * overlay.c — 原生浮层（Main 进程）
 *
 * 为什么要有它（docs/07 §7.19–7.22）：
 *   CEF 的 JS 执行 API 在 3.1.42 上是空操作；scheme factory 也从不被调用
 *   （页面资源在 package\orpheus.ntpk 里，由宿主自己的一条内部路径提供）。
 *   在把资源链路彻底搞清之前，先给插件宿主一个**确定可见**的 UI：
 *   主进程用原生窗口画一条置顶浮层，不碰 CEF，也不依赖页面 DOM。
 *
 *   WS_EX_TRANSPARENT 让它鼠标穿透（不挡操作），
 *   WS_EX_TOOLWINDOW 让它在任务栏不留条目，
 *   每 1 秒跟随网易云主窗口位置重新贴边。
 */
#include "overlay.h"
#include "util.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define OVERLAY_CLASS L"NcmPluginOverlay"
#define OVERLAY_W     340
#define OVERLAY_H     30
#define WM_OVERLAY_UPDATE (WM_APP + 41)

static HWND        g_hwnd = NULL;
static HANDLE      g_thread = NULL;
static volatile LONG g_stop = 0;
static char        g_text[256] = "NCM Plugin Host";

/* 找宿主主窗口（同进程、可见、有标题、无 owner） */
/* 找宿主主窗口。
 *
 * ★ 必须只认「真正的宿主窗口」，不能把**我们自己**的浮层当成宿主：
 *   实测当网易云窗口隐藏（托盘/最小化/还在初始化）时，Windows 会把我们的
 *   浮层报成进程的 MainWindow，于是出现"网易云没启动但屏幕上有个浮窗"的怪象。
 *   这里优先认 class = OrpheusBrowserHost，其次才是"本进程、可见、有标题、
 *   面积够大、且不是我们自己的类"的窗口。 */
static HWND find_host_window(void)
{
    struct ctx { DWORD pid; HWND best; int area; HWND by_class; } c;
    c.pid = GetCurrentProcessId();
    c.best = NULL;
    c.area = 0;
    c.by_class = NULL;

    HWND h = GetTopWindow(NULL);
    while (h) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == c.pid) {
            wchar_t cls[128] = {0};
            GetClassNameW(h, cls, 128);

            /* 我们自己，跳过 */
            if (wcscmp(cls, OVERLAY_CLASS) != 0) {
                /* ★ 网易云的 UI 是 CEF 的 Chrome_WidgetWin_*，而且**没有标题**。
                 *   以前这里要求"有标题"，导致永远找不到宿主、浮层掉到屏幕角落
                 *   （正是"网易云没打开却有个浮窗"的直接原因之一）。
                 *   现在按「类名 + 可见 + 未最小化 + 面积」判定。 */
                if (wcscmp(cls, L"OrpheusBrowserHost") == 0) c.by_class = h;
                int looks_like_ui = (wcsncmp(cls, L"Chrome_WidgetWin", 16) == 0) ||
                                    (wcscmp(cls, L"OrpheusBrowserHost") == 0);
                if (looks_like_ui && IsWindowVisible(h) && !IsIconic(h) &&
                    GetWindow(h, GW_OWNER) == NULL) {
                    RECT r;
                    if (GetWindowRect(h, &r)) {
                        int w = r.right - r.left, hh = r.bottom - r.top;
                        if (w >= 300 && hh >= 300) {
                            int area = w * hh;
                            if (area > c.area) { c.area = area; c.best = h; }
                        }
                    }
                }
            }
        }
        h = GetWindow(h, GW_HWNDNEXT);
    }
    if (c.by_class && IsWindowVisible(c.by_class) && !IsIconic(c.by_class)) return c.by_class;
    return c.best;
}

/* 宿主是否真的在屏幕上（可见、未最小化、有实际尺寸） */
static int host_on_screen(HWND host, RECT *out)
{
    if (!host) return 0;
    if (!IsWindowVisible(host)) return 0;
    if (IsIconic(host)) return 0;
    RECT r;
    if (!GetWindowRect(host, &r)) return 0;
    if ((r.right - r.left) < 200 || (r.bottom - r.top) < 200) return 0;
    if (out) *out = r;
    return 1;
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    /* 背景：深红圆角块 */
    HBRUSH bg = CreateSolidBrush(RGB(198, 47, 47));
    HBRUSH old = (HBRUSH)SelectObject(dc, bg);
    RoundRect(dc, 0, 0, rc.right, rc.bottom, 8, 8);
    SelectObject(dc, old);
    DeleteObject(bg);

    /* 文字：白色，垂直居中 */
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT f = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Microsoft YaHei UI");
    HFONT of = (HFONT)SelectObject(dc, f);

    wchar_t w[256];
    MultiByteToWideChar(CP_UTF8, 0, g_text, -1, w, 256);
    RECT tr = rc;
    tr.left = 10;
    DrawTextW(dc, w, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, of);
    DeleteObject(f);
    EndPaint(hwnd, &ps);
}

static void reposition(void)
{
    if (!g_hwnd) return;

    RECT hr;
    HWND host = find_host_window();

    /* ★ 宿主不在屏幕上（未启动/托盘/最小化/还在初始化）就**藏起来**。
     *   以前这里回退到屏幕角落，导致"网易云没开却有个浮窗挂在桌面右上角"。 */
    if (!host_on_screen(host, &hr)) {
        if (IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_HIDE);
        return;
    }

    int x = hr.right - OVERLAY_W - 16;
    int y = hr.top + 44;
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, OVERLAY_W, OVERLAY_H,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_OVERLAY_UPDATE:
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_TIMER:
        reposition();
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI overlay_thread(LPVOID param)
{
    (void)param;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);

    /* 无边框、置顶、工具窗、鼠标穿透、不激活 */
    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
        WS_EX_LAYERED,
        OVERLAY_CLASS, NULL, WS_POPUP,   /* 不设标题：避免被「窗口标题徽标」误改成宿主 */
        0, 0, OVERLAY_W, OVERLAY_H, NULL, NULL, wc.hInstance, NULL);

    if (!g_hwnd) {
        ncm_log("[overlay] ❌ CreateWindowEx 失败 err=%lu", GetLastError());
        return 0;
    }
    SetLayeredWindowAttributes(g_hwnd, 0, 235, LWA_ALPHA);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    reposition();
    InvalidateRect(g_hwnd, NULL, TRUE);
    UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, 1, 1000, NULL);
    ncm_log("[overlay] ✅ 浮层已创建 hwnd=%p 文本=\"%s\"", g_hwnd, g_text);

    MSG msg;
    while (!g_stop && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hwnd) { KillTimer(g_hwnd, 1); DestroyWindow(g_hwnd); g_hwnd = NULL; }
    return 0;
}

void ncm_overlay_start(const char *text)
{
    if (text && text[0]) {
        strncpy_s(g_text, sizeof(g_text), text, _TRUNCATE);
    }
    if (g_thread) return;
    g_thread = CreateThread(NULL, 0, overlay_thread, NULL, 0, NULL);
    if (!g_thread) ncm_log("[overlay] ❌ 线程创建失败 err=%lu", GetLastError());
}

void ncm_overlay_set_text(const char *text)
{
    if (!text) return;
    strncpy_s(g_text, sizeof(g_text), text, _TRUNCATE);
    if (g_hwnd) PostMessageW(g_hwnd, WM_OVERLAY_UPDATE, 0, 0);
}

void ncm_overlay_stop(void)
{
    InterlockedExchange(&g_stop, 1);
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 1000); CloseHandle(g_thread); g_thread = NULL; }
}
