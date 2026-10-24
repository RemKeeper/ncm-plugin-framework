/**
 * bcast.c — 局域网投送（对齐 go-musicfox 的 broadcast 协议）
 *
 * 端点：
 *   GET  /events   Server-Sent Events 流，推送 event: nowplaying / data: <JSON>
 *   GET  /cover    当前封面图片（内容类型原样返回，本地缓存）
 *   POST /publish  页面插件推送上来的快照 JSON（仅回环访问）
 *   POST /cover    页面插件推送上来的封面字节（仅回环访问，?type=image/jpeg）
 *
 * 为什么服务端放在原生：页面 JS 没有监听端口的能力，只有原生能做 TCP 服务。
 * 页面插件负责"采集 + 推送"，原生负责"扇出给局域网客户端"。
 *
 * 配置：%LOCALAPPDATA%\NCMPlugin\broadcast.json
 *   { "enable": true, "port": 8765, "bind": "" }
 */
/* winsock2 必须在 windows.h 之前（probe_server.c 已踩过同样的坑） */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "bcast.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef NCM_ENABLE_BCAST
#define NCM_ENABLE_BCAST 1
#endif

#define BCAST_DEFAULT_PORT 8765

/* ── 状态 ─────────────────────────────────────────────────────────────── */
static SOCKET            g_listen = INVALID_SOCKET;
static CRITICAL_SECTION  g_lock;
static volatile LONG     g_inited = 0;

static char             *g_snapshot = NULL;      /* 最近一次快照 JSON */
static unsigned char    *g_cover = NULL;
static int               g_cover_len = 0;
static char              g_cover_type[64] = "image/jpeg";

static SOCKET            g_clients[16];
static int               g_client_count = 0;
static DWORD             g_last_pub = 0;      /* 节流：上次广播时刻 */
static long              g_pub_dropped = 0;

static int g_port = BCAST_DEFAULT_PORT;
static int g_active_port = 0;
static char g_bind[64] = "";                     /* 空 = 0.0.0.0 */

/* ── 配置读取（极简 JSON 取值，够用） ─────────────────────────────────── */
static void read_config(void)
{
    const char *la = getenv("LOCALAPPDATA");
    if (!la) return;
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\NCMPlugin\\broadcast.json", la);

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[1024] = {0};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, NULL);
    CloseHandle(h);
    buf[rd] = 0;

    const char *p = strstr(buf, "\"port\"");
    if (p) {
        p = strchr(p, ':');
        if (p) { int v = atoi(p + 1); if (v > 1024 && v < 65535) g_port = v; }
    }
    p = strstr(buf, "\"bind\"");
    if (p) {
        const char *q = strchr(p + 6, '"');
        const char *r = q ? strchr(++q, '"') : NULL;
        if (q && r && r > q) {
            int n = (int)(r - q);
            if (n > 0 && n < (int)sizeof(g_bind)) { memcpy(g_bind, q, n); g_bind[n] = 0; }
        }
    }
}

/* ── 客户端管理（锁内操作） ───────────────────────────────────────────── */
static void client_add(SOCKET s)
{
    EnterCriticalSection(&g_lock);
    if (g_client_count < (int)(sizeof(g_clients) / sizeof(g_clients[0]))) {
        g_clients[g_client_count++] = s;
    }
    LeaveCriticalSection(&g_lock);
}

static void client_remove(SOCKET s)
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_client_count; i++) {
        if (g_clients[i] == s) {
            g_clients[i] = g_clients[g_client_count - 1];
            g_client_count--;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

/* 把一条事件发给所有 SSE 客户端；失败的顺手摘掉 */
static void broadcast_event(const char *json)
{
    if (!json) return;
    char *msg = (char *)malloc(strlen(json) + 64);
    if (!msg) return;
    int n = sprintf_s(msg, strlen(json) + 64,
                      "event: nowplaying\ndata: %s\n\n", json);

    EnterCriticalSection(&g_lock);
    for (int i = g_client_count - 1; i >= 0; i--) {
        if (send(g_clients[i], msg, n, 0) <= 0) {
            closesocket(g_clients[i]);
            g_clients[i] = g_clients[g_client_count - 1];
            g_client_count--;
        }
    }
    LeaveCriticalSection(&g_lock);
    free(msg);
}

/* 把快照里的 "seq" 换成原生侧单调递增的编号。
 * 为什么要这样：插件在多个文档里各跑一份，各自计数会导致客户端看到
 * seq 忽大忽小（实测 3,6,4,5,8,6,9），无法用于判重。 */
static long g_seq = 0;

static char *bump_seq(const char *json)
{
    if (!json) return NULL;
    const char *p = strstr(json, "\"seq\"");
    if (!p) return _strdup(json);

    const char *colon = strchr(p, ':');
    if (!colon) return _strdup(json);
    const char *num = colon + 1;
    while (*num == ' ') num++;
    const char *end = num;
    while (*end == '-' || (*end >= '0' && *end <= '9')) end++;

    char numbuf[32];
    int n = sprintf_s(numbuf, sizeof(numbuf), "%ld", InterlockedIncrement(&g_seq));

    size_t head = (size_t)(num - json);
    size_t tail = strlen(end);
    char *out = (char *)malloc(head + (size_t)n + tail + 1);
    if (!out) return _strdup(json);
    memcpy(out, json, head);
    memcpy(out + head, numbuf, (size_t)n);
    memcpy(out + head + n, end, tail + 1);
    return out;
}

/* ── SSE 连接线程 ────────────────────────────────────────────────────── */
static DWORD WINAPI sse_thread(LPVOID param)
{
    SOCKET s = (SOCKET)(uintptr_t)param;

    const char *hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/event-stream; charset=utf-8\r\n"
                      "Cache-Control: no-cache\r\n"
                      "Connection: keep-alive\r\n"
                      "Access-Control-Allow-Origin: *\r\n\r\n";
    if (send(s, hdr, (int)strlen(hdr), 0) <= 0) { closesocket(s); return 0; }

    /* 连上立刻推一次当前快照 */
    EnterCriticalSection(&g_lock);
    char *snap = g_snapshot ? _strdup(g_snapshot) : NULL;
    LeaveCriticalSection(&g_lock);
    if (snap) {
        char *msg = (char *)malloc(strlen(snap) + 64);
        if (msg) {
            int n = sprintf_s(msg, strlen(snap) + 64,
                              "event: nowplaying\ndata: %s\n\n", snap);
            send(s, msg, n, 0);
            free(msg);
        }
        free(snap);
    }

    client_add(s);
    ncm_trace("bcast: SSE 客户端接入（当前 %d 个）", g_client_count);

    DWORD last_keepalive = GetTickCount();
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 300000;                 /* 300ms 轮一次 */
        int r = select(0, &rd, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(s, &rd)) {
            char tmp[256];
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) break;               /* 客户端断开 */
        }
        if (GetTickCount() - last_keepalive > 15000) {
            const char *ka = ": keep-alive\n\n";
            if (send(s, ka, (int)strlen(ka), 0) <= 0) break;
            last_keepalive = GetTickCount();
        }
    }

    client_remove(s);
    closesocket(s);
    ncm_trace("bcast: SSE 客户端断开（剩 %d 个）", g_client_count);
    return 0;
}

/* ── 请求处理 ────────────────────────────────────────────────────────── */
static void send_simple(SOCKET s, int code, const char *ctype, const char *body, int blen)
{
    char hdr[320];
    int n = sprintf_s(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                      "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                      code, code == 200 ? "OK" : (code == 204 ? "No Content" : "Error"),
                      ctype, blen);
    send(s, hdr, n, 0);
    if (blen > 0 && body) send(s, body, blen, 0);
    closesocket(s);
}

static void handle_conn(SOCKET s)
{
    char buf[8192];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (total <= 0) { closesocket(s); return; }

    int is_get  = (strncmp(buf, "GET ", 4) == 0);
    int is_post = (strncmp(buf, "POST ", 5) == 0);

    /* ── GET /events ── */
    if (is_get && strstr(buf, "GET /events")) {
        HANDLE h = CreateThread(NULL, 0, sse_thread, (LPVOID)(uintptr_t)s, 0, NULL);
        if (h) CloseHandle(h);
        else closesocket(s);
        return;
    }

    /* ── GET /cover ── */
    if (is_get && strstr(buf, "GET /cover")) {
        EnterCriticalSection(&g_lock);
        int len = g_cover_len;
        unsigned char *copy = NULL;
        if (len > 0 && g_cover) {
            copy = (unsigned char *)malloc(len);
            if (copy) memcpy(copy, g_cover, len);
        }
        char type[64];
        strcpy_s(type, sizeof(type), g_cover_type);
        LeaveCriticalSection(&g_lock);

        if (!copy) { send_simple(s, 204, "text/plain", "", 0); return; }

        char hdr[320];
        int n = sprintf_s(hdr, sizeof(hdr),
                          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                          "Cache-Control: max-age=86400\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                          type, len);
        send(s, hdr, n, 0);
        send(s, (const char *)copy, len, 0);
        free(copy);
        closesocket(s);
        return;
    }

    /* ── POST /publish | /cover ── */
    if (is_post) {
        int content_len = 0;
        const char *cl = strstr(buf, "Content-Length:");
        if (!cl) cl = strstr(buf, "content-length:");
        if (cl) content_len = atoi(cl + 15);
        if (content_len < 0) content_len = 0;
        if (content_len > 4 * 1024 * 1024) content_len = 4 * 1024 * 1024;

        const char *body = strstr(buf, "\r\n\r\n");
        int have = 0;
        if (body) { body += 4; have = total - (int)(body - buf); if (have < 0) { have = 0; body = buf + total; } }
        else { body = buf + total; }

        char *payload = (char *)malloc((size_t)content_len + 1);
        if (!payload) { closesocket(s); return; }
        int got = 0;
        if (have > 0) { got = have < content_len ? have : content_len; memcpy(payload, body, (size_t)got); }
        while (got < content_len) {
            int n = recv(s, payload + got, content_len - got, 0);
            if (n <= 0) break;
            got += n;
        }
        payload[got] = 0;

        if (strstr(buf, "POST /publish")) {
            /* ★ 节流：载荷被注入到多个 JS chunk，每个文档都会跑一份插件，
             *   实测 9 秒能收到 171 次推送。这里限制为最快 900ms 一次。 */
            DWORD now = GetTickCount();
            int accept = 0;
            EnterCriticalSection(&g_lock);
            if (g_last_pub == 0 || (now - g_last_pub) >= 900) {
                g_last_pub = now;
                accept = 1;
                if (g_snapshot) { free(g_snapshot); g_snapshot = NULL; }
                g_snapshot = _strdup(payload);   /* 存原文，广播时再编号 */
            } else {
                g_pub_dropped++;
            }
            char *snap = (accept && g_snapshot) ? bump_seq(g_snapshot) : NULL;
            LeaveCriticalSection(&g_lock);

            if (snap) { broadcast_event(snap); free(snap); }
            free(payload);
            send_simple(s, 204, "text/plain", "", 0);
            return;
        }

        if (strstr(buf, "POST /cover")) {
            const char *t = strstr(buf, "?type=");
            char type[64] = "image/jpeg";
            if (t) {
                t += 6;
                int i = 0;
                while (t[i] && t[i] != ' ' && t[i] != '&' && i < 60) { type[i] = t[i]; i++; }
                type[i] = 0;
            }
            EnterCriticalSection(&g_lock);
            if (g_cover) { free(g_cover); g_cover = NULL; g_cover_len = 0; }
            if (got > 0) {
                g_cover = (unsigned char *)malloc((size_t)got);
                if (g_cover) { memcpy(g_cover, payload, (size_t)got); g_cover_len = got; }
            }
            strcpy_s(g_cover_type, sizeof(g_cover_type), type);
            LeaveCriticalSection(&g_lock);
            ncm_trace("bcast: 封面已更新（%d 字节，%s）", got, type);
            free(payload);
            send_simple(s, 204, "text/plain", "", 0);
            return;
        }

        free(payload);
        send_simple(s, 404, "text/plain", "not found", 9);
        return;
    }

    send_simple(s, 404, "text/plain", "not found", 9);
}

/* 每个连接一个线程：插件用的是 keep-alive 长连接，
 * 如果和 accept 在同一个线程里同步处理，会把监听循环卡死（实测 SSE 连不进来）。 */
static DWORD WINAPI conn_thread(LPVOID param)
{
    SOCKET s = (SOCKET)(uintptr_t)param;
    handle_conn(s);
    return 0;
}

/* ── 监听线程 ────────────────────────────────────────────────────────── */
static DWORD WINAPI listen_thread(LPVOID param)
{
    (void)param;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        ncm_trace("bcast: WSAStartup 失败");
        return 0;
    }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) return 0;

    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)g_port);
    addr.sin_addr.s_addr = (g_bind[0] == 0) ? htonl(INADDR_ANY) : inet_addr(g_bind);

    /* 端口可能被别的程序占用（实测用户的 go-musicfox 就占着 8765），
     * 所以在 8765..8774 里找一个能绑的。 */
    int bound = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        addr.sin_port = htons((u_short)(g_port + attempt));
        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            g_active_port = g_port + attempt;
            bound = 1;
            break;
        }
    }
    if (!bound) {
        ncm_trace("bcast: bind %s:%d..%d 全部失败 err=%d",
                  g_bind[0] ? g_bind : "0.0.0.0", g_port, g_port + 9, WSAGetLastError());
        closesocket(srv);
        return 0;
    }
    if (listen(srv, 8) == SOCKET_ERROR) { closesocket(srv); return 0; }

    g_listen = srv;
    ncm_trace("bcast: ✅ 局域网投送已启动 http://%s:%d/events（SSE）",
              g_bind[0] ? g_bind : "0.0.0.0", g_active_port);

    for (;;) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        HANDLE h = CreateThread(NULL, 0, conn_thread, (LPVOID)(uintptr_t)c, 0, NULL);
        if (h) CloseHandle(h);
        else closesocket(c);
    }
    return 0;
}

void ncm_bcast_start(void)
{
#if NCM_ENABLE_BCAST
    if (InterlockedExchange(&g_inited, 1)) return;
    InitializeCriticalSection(&g_lock);
    read_config();

    /* enable=false 时不启动 */
    const char *la = getenv("LOCALAPPDATA");
    if (la) {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\NCMPlugin\\broadcast.json", la);
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[256] = {0};
            DWORD rd = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &rd, NULL);
            CloseHandle(h);
            if (strstr(buf, "\"enable\"") && strstr(buf, "false")) {
                ncm_trace("bcast: 配置 enable=false，不启动");
                return;
            }
        }
    }

    HANDLE h = CreateThread(NULL, 0, listen_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
#endif
}
/* 给 probe_server 用：把实际监听端口告诉页面插件（8765 可能被占用） */
int ncm_bcast_port(void) { return g_active_port; }
/* 通知所有 SSE 客户端：插件有变更，请热重载。
 * 为什么不用页面轮询：窗口不可见时 Chromium 会冻结定时器（实测 polls=0），
 * 而 SSE 是网络事件，不受该限制。 */
void ncm_bcast_notify_reload(void)
{
    char msg[128];
    int n = sprintf_s(msg, sizeof(msg), "event: reload\ndata: {\"at\":%lu}\n\n",
                      (unsigned long)GetTickCount());
    EnterCriticalSection(&g_lock);
    for (int i = g_client_count - 1; i >= 0; i--) {
        if (send(g_clients[i], msg, n, 0) <= 0) {
            closesocket(g_clients[i]);
            g_clients[i] = g_clients[g_client_count - 1];
            g_client_count--;
        }
    }
    LeaveCriticalSection(&g_lock);
}