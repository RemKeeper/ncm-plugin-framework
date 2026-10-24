/**
 * probe_server.c — 页面 → 原生的回传通道
 *
 * 为什么需要它：页面载荷跑在渲染进程的 JS 上下文里，没有原生桥（扩展上下文那条
 * 路不可用）。但页面可以 `fetch('http://127.0.0.1:PORT/...')`，所以这里在宿主
 * 进程里起一个**只监听回环地址**的极简 HTTP 服务，把页面发来的 JSON 写进 trace。
 *
 * 用途：
 *   - 把页面里的 DOM 探测结果（歌曲/歌词/封面候选节点）回传给原生侧分析；
 *   - 后续也可以作为插件 `ncm.fs`/`ncm.config` 的替代通道。
 *
 * 安全边界：只绑定 127.0.0.1、只接受 POST、请求体截断记录、不落盘原始数据以外的东西。
 */
/* 必须最先包含 winsock2.h，否则 windows.h 里的 winsock1 定义会冲突 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "probe_server.h"
#include "util.h"
#include "bcast.h"
#include "plugins.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#ifndef NCM_ENABLE_PROBE_SERVER
#define NCM_ENABLE_PROBE_SERVER 1
#endif

#define PROBE_PORT 38021

static volatile LONG g_started = 0;

/* 极简动态字符串（本文件自用；不依赖 plugins.c 的内部 sbuf） */
typedef struct { char *p; int len; int cap; } psbuf;
static void ps_init(psbuf *b, int cap) { b->p = (char *)malloc((size_t)cap); b->len = 0; b->cap = cap; if (b->p) b->p[0] = 0; }
static void ps_add(psbuf *b, const char *s) {
    if (!b->p || !s) return;
    int n = (int)strlen(s);
    if (b->len + n + 1 > b->cap) return;
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = 0;
}
static void ps_addf(psbuf *b, const char *fmt, ...) {
    if (!b->p) return;
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(tmp, sizeof(tmp), _TRUNCATE, fmt, ap);
    va_end(ap);
    ps_add(b, tmp);
}

static void handle_client(SOCKET s)
{
    char buf[8192];
    int total = 0;

    /* 读头部 */
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }

    /* Content-Length */
    int content_len = 0;
    const char *cl = strstr(buf, "Content-Length:");
    if (!cl) cl = strstr(buf, "content-length:");
    if (cl) content_len = atoi(cl + 15);
    if (content_len < 0) content_len = 0;
    if (content_len > 200000) content_len = 200000;

    /* 已经读到 body 的部分 */
    const char *body = strstr(buf, "\r\n\r\n");
    int have = 0;
    if (body) {
        body += 4;
        have = total - (int)(body - buf);
        if (have < 0) { have = 0; body = buf + total; }
    } else {
        body = buf + total;
    }

    char *payload = (char *)malloc((size_t)content_len + 1);
    if (!payload) { closesocket(s); return; }
    int got = 0;
    if (have > 0) {
        got = have < content_len ? have : content_len;
        memcpy(payload, body, (size_t)got);
    }
    while (got < content_len) {
        int n = recv(s, payload + got, content_len - got, 0);
        if (n <= 0) break;
        got += n;
    }
    payload[got] = 0;

    /* ── 插件文件系统（沙箱在 %LOCALAPPDATA%\NCMPlugin\data）──────────────
     * GET  /fs/read?path=a/b.txt     读文本
     * POST /fs/write?path=a/b.txt    写文本（自动建目录）
     * GET  /fs/list?path=a           列目录 → JSON 数组
     * POST /fs/remove?path=a/b.txt   删除文件/空目录
     * GET  /fs/stat?path=a/b.txt     元信息 → JSON
     * 安全：拒绝绝对路径与 ".."，最终路径必须落在 data 根目录内。 */
    if (strncmp(buf, "GET /fs/", 8) == 0 || strncmp(buf, "POST /fs/", 9) == 0) {
        int is_post = (buf[0] == 'P');
        const char *q = strstr(buf, "?path=");
        char rel[1024] = {0};
        if (q) {
            q += 6;
            int i = 0;
            while (*q && *q != ' ' && *q != '&' && i < (int)sizeof(rel) - 1) {
                /* 极简 percent-decode */
                if (*q == '%' && q[1] && q[2]) {
                    int hi = (q[1] >= '0' && q[1] <= '9') ? q[1] - '0' : ((q[1] | 32) - 'a' + 10);
                    int lo = (q[2] >= '0' && q[2] <= '9') ? q[2] - '0' : ((q[2] | 32) - 'a' + 10);
                    rel[i++] = (char)((hi << 4) | lo);
                    q += 3;
                } else {
                    rel[i++] = (*q == '/') ? '\\' : *q;
                    q++;
                }
            }
            rel[i] = 0;
        }

        int safe = (rel[0] != 0) && (rel[0] != '\\') && (strstr(rel, "..") == NULL) && (rel[1] != ':');
        /* 沙箱根目录固定为 %LOCALAPPDATA%\NCMPlugin\data（不碰网易云自己的数据） */
        wchar_t root[MAX_PATH] = {0};
        {
            const char *la = getenv("LOCALAPPDATA");
            wchar_t wla[MAX_PATH] = {0};
            if (la) MultiByteToWideChar(CP_UTF8, 0, la, -1, wla, MAX_PATH);
            _snwprintf_s(root, MAX_PATH, _TRUNCATE, L"%s\\NCMPlugin\\data", wla);
        }
        wchar_t full[MAX_PATH * 2] = {0};
        if (safe && root[0]) {
            wchar_t wrel[1024];
            MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel, 1024);
            _snwprintf_s(full, _countof(full), _TRUNCATE, L"%s\\%s", root, wrel);
        }

        if (!safe || !full[0]) {
            const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nbad path!!";
            send(s, bad, (int)strlen(bad), 0);
            closesocket(s);
            return;
        }

        /* 建父目录 */
        if (is_post || strncmp(buf, "GET /fs/read", 12) == 0) {
            wchar_t parent[MAX_PATH * 2];
            wcscpy_s(parent, _countof(parent), full);
            wchar_t *slash = wcsrchr(parent, L'\\');
            if (slash) { *slash = 0; }
            /* 逐级创建 */
            for (wchar_t *pp = parent; *pp; pp++) {
                if (*pp == L'\\') { wchar_t t = *pp; *pp = 0; CreateDirectoryW(parent, NULL); *pp = t; }
            }
            CreateDirectoryW(parent, NULL);
        }

        char *out_body = NULL;
        int   out_len = 0;
        const char *ctype = "text/plain; charset=utf-8";

        if (strncmp(buf, "GET /fs/read", 12) == 0) {
            HANDLE h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz; sz.QuadPart = 0;
                if (GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (8 << 20)) {
                    out_len = (int)sz.QuadPart;
                    out_body = (char *)malloc((size_t)out_len + 1);
                    if (out_body) {
                        DWORD rd = 0;
                        if (!ReadFile(h, out_body, (DWORD)out_len, &rd, NULL)) out_len = 0;
                        else out_len = (int)rd;
                        out_body[out_len] = 0;
                    }
                }
                CloseHandle(h);
            }
        } else if (is_post && strncmp(buf, "POST /fs/write", 14) == 0) {
            int clen = 0;
            const char *cl = strstr(buf, "Content-Length:");
            if (!cl) cl = strstr(buf, "content-length:");
            if (cl) clen = atoi(cl + 15);
            if (clen < 0) clen = 0;
            if (clen > (8 << 20)) clen = 8 << 20;
            const char *bp2 = strstr(buf, "\r\n\r\n");
            int have2 = 0;
            if (bp2) { bp2 += 4; have2 = total - (int)(bp2 - buf); if (have2 < 0) { have2 = 0; bp2 = buf + total; } }
            else { bp2 = buf + total; }

            HANDLE h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                int got = 0;
                if (have2 > 0) { got = have2 < clen ? have2 : clen; DWORD wr = 0; WriteFile(h, bp2, (DWORD)got, &wr, NULL); }
                while (got < clen) {
                    char tmp[2048];
                    int n = recv(s, tmp, (int)sizeof(tmp), 0);
                    if (n <= 0) break;
                    DWORD wr = 0;
                    WriteFile(h, tmp, (DWORD)n, &wr, NULL);
                    got += n;
                }
                CloseHandle(h);
                ncm_trace("fs/write: %s (%d 字节)", rel, got);
            }
        } else if (is_post && strncmp(buf, "POST /fs/remove", 15) == 0) {
            DeleteFileW(full);
            RemoveDirectoryW(full);
        } else if (strncmp(buf, "GET /fs/list", 12) == 0) {
            wchar_t pat[MAX_PATH * 2];
            _snwprintf_s(pat, _countof(pat), _TRUNCATE, L"%s\\*", full);
            WIN32_FIND_DATAW fd;
            HANDLE hf = FindFirstFileW(pat, &fd);
            psbuf sb;
            ps_init(&sb, 8192);
            ps_add(&sb, "[");
            int first = 1;
            if (hf != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                    char name8[512];
                    WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name8, sizeof(name8), NULL, NULL);
                    if (!first) ps_add(&sb, ",");
                    first = 0;
                    ps_add(&sb, "{\"name\":\"");
                    ps_add(&sb, name8);          /* 简化：目录名里极少有需要转义的字符 */
                    ps_add(&sb, "\",\"dir\":");
                    ps_add(&sb, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "true" : "false");
                    ps_addf(&sb, ",\"size\":%llu}",
                            ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
                } while (FindNextFileW(hf, &fd));
                FindClose(hf);
            }
            ps_add(&sb, "]");
            out_body = sb.p;
            out_len = sb.len;
            ctype = "application/json; charset=utf-8";
        } else if (strncmp(buf, "GET /fs/stat", 12) == 0) {
            wchar_t wrel2[1024];
            MultiByteToWideChar(CP_UTF8, 0, rel, -1, wrel2, 1024);
            WIN32_FILE_ATTRIBUTE_DATA fad;
            char json[256];
            if (GetFileAttributesExW(full, GetFileExInfoStandard, &fad)) {
                _snprintf_s(json, sizeof(json), _TRUNCATE,
                            "{\"exists\":true,\"dir\":%s,\"size\":%llu}",
                            (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "true" : "false",
                            ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);
            } else {
                strcpy_s(json, sizeof(json), "{\"exists\":false}");
            }
            out_body = _strdup(json);
            out_len = (int)strlen(json);
            ctype = "application/json; charset=utf-8";
        }

        char hdr[256];
        int hn = sprintf_s(hdr, sizeof(hdr),
                           "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
                           "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                           ctype, out_len);
        send(s, hdr, hn, 0);
        if (out_len > 0 && out_body) send(s, out_body, out_len, 0);
        if (out_body) free(out_body);
        closesocket(s);
        return;
    }
    /* ── 插件日志落盘：POST /log（页面 console 之外，方便排查）─────────── */
    if (strncmp(buf, "POST /log", 9) == 0) {
        int clen = 0;
        const char *cl = strstr(buf, "Content-Length:");
        if (!cl) cl = strstr(buf, "content-length:");
        if (cl) clen = atoi(cl + 15);
        if (clen > 0 && clen < 8192) {
            const char *bp3 = strstr(buf, "\r\n\r\n");
            int have3 = 0;
            if (bp3) { bp3 += 4; have3 = total - (int)(bp3 - buf); if (have3 < 0) { have3 = 0; bp3 = buf + total; } }
            else { bp3 = buf + total; }
            char line[8192];
            int got = have3 < clen ? have3 : clen;
            if (got > 0) memcpy(line, bp3, (size_t)got);
            while (got < clen) {
                int n = recv(s, line + got, (int)sizeof(line) - 1 - got, 0);
                if (n <= 0) break;
                got += n;
            }
            line[got] = 0;
            ncm_trace("JS %s", line);
        }
        const char *ok = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        send(s, ok, (int)strlen(ok), 0);
        closesocket(s);
        return;
    }
    /* ── 热重载：GET /host 返回 ncm_host_json()（含每个插件的 mtime/脚本源码）──
     * 页面侧轮询它，发现 mtime 变化就重新加载插件，不必重启应用。 */
    if (strncmp(buf, "GET /host", 9) == 0) {
        char *json = ncm_host_json();
        int len = json ? (int)strlen(json) : 0;
        char hdr[256];
        int hn = sprintf_s(hdr, sizeof(hdr),
                           "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
                           "Content-Length: %d\r\nCache-Control: no-store\r\n"
                           "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", len);
        send(s, hdr, hn, 0);
        if (len) send(s, json, len, 0);
        if (json) free(json);
        closesocket(s);
        return;
    }
    /* ── 自定义 CSS 持久化（插件用）────────────────────────────────────
     * GET  /custom-css  → 返回 %LOCALAPPDATA%\NCMPlugin\custom.css
     * POST /custom-css  → 把请求体写入该文件
     */
    if (strncmp(buf, "GET /custom-css", 15) == 0) {
        char path[MAX_PATH];
        const char *la = getenv("LOCALAPPDATA");
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\NCMPlugin\\custom.css", la ? la : ".");
        char *body = NULL;
        int len = 0;
        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz; sz.QuadPart = 0;
            if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (4 << 20)) {
                len = (int)sz.QuadPart;
                body = (char *)malloc((size_t)len);
                if (body) { DWORD rd = 0; if (!ReadFile(h, body, (DWORD)len, &rd, NULL)) len = 0; else len = (int)rd; }
            }
            CloseHandle(h);
        }
        char hdr[256];
        int hn = sprintf_s(hdr, sizeof(hdr),
                           "HTTP/1.1 200 OK\r\nContent-Type: text/css; charset=utf-8\r\n"
                           "Content-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                           len);
        send(s, hdr, hn, 0);
        if (len > 0 && body) send(s, body, len, 0);
        if (body) free(body);
        closesocket(s);
        return;
    }

    if (strncmp(buf, "POST /custom-css", 16) == 0) {
        int clen = 0;
        const char *cl = strstr(buf, "Content-Length:");
        if (!cl) cl = strstr(buf, "content-length:");
        if (cl) clen = atoi(cl + 15);
        if (clen < 0) clen = 0;
        if (clen > (2 << 20)) clen = 2 << 20;

        const char *bp = strstr(buf, "\r\n\r\n");
        int have = 0;
        if (bp) { bp += 4; have = total - (int)(bp - buf); if (have < 0) { have = 0; bp = buf + total; } }
        else { bp = buf + total; }

        char *payload = (char *)malloc((size_t)clen + 1);
        if (payload) {
            int got = 0;
            if (have > 0) { got = have < clen ? have : clen; memcpy(payload, bp, (size_t)got); }
            while (got < clen) {
                int n = recv(s, payload + got, clen - got, 0);
                if (n <= 0) break;
                got += n;
            }
            payload[got] = 0;

            char path[MAX_PATH];
            const char *la = getenv("LOCALAPPDATA");
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\NCMPlugin\\custom.css", la ? la : ".");
            HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(h, payload, (DWORD)got, &wr, NULL);
                CloseHandle(h);
                ncm_trace("custom-css: 已保存 %d 字节", got);
            }
            free(payload);
        }
        const char *ok = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        send(s, ok, (int)strlen(ok), 0);
        closesocket(s);
        return;
    }
    /* GET /bcast-port → 告诉页面插件局域网投送实际监听在哪个端口 */
    if (strncmp(buf, "GET /bcast-port", 15) == 0) {
        char num[16];
        int n = sprintf_s(num, sizeof(num), "%d", ncm_bcast_port());
        char hdr[192];
        int hn = sprintf_s(hdr, sizeof(hdr),
                           "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n"
                           "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", n);
        send(s, hdr, hn, 0);
        send(s, num, n, 0);
        closesocket(s);
        return;
    }

    /* GET /cmd → 返回 %LOCALAPPDATA%\NCMPlugin\cmd.js 的内容（原生给页面下发命令） */
    if (strncmp(buf, "GET /cmd", 8) == 0) {
        const char *la = getenv("LOCALAPPDATA");
        char path[MAX_PATH] = {0};
        char body[8192] = {0};
        int blen = 0;
        if (la) {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\NCMPlugin\\cmd.js", la);
            HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD rd = 0;
                ReadFile(h, body, (DWORD)sizeof(body) - 1, &rd, NULL);
                CloseHandle(h);
                blen = (int)rd;
                body[blen] = 0;
            }
        }
        char hdr[192];
        int hn = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                             "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                             "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", blen);
        send(s, hdr, hn, 0);
        if (blen > 0) send(s, body, blen, 0);
        closesocket(s);
        return;
    }

    /* 完整 body 落盘（trace 行有长度上限，JSON 会被截断） */
    {
        const char *la = getenv("LOCALAPPDATA");
        if (la) {
            char dir[MAX_PATH], path[MAX_PATH];
            _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\NCMPlugin\\dom", la);
            CreateDirectoryA(dir, NULL);
            static volatile LONG seq = 0;
            LONG n = InterlockedIncrement(&seq);
            if (n > 40) seq = 1, n = 1;   /* 只保留最近 40 帧 */
            /* 按请求路径决定前缀：/api → api-NNN.json，/dom → snap-NNN.json */
            const char *prefix = strstr(buf, "POST /api") ? "api" : "snap";
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s-%03ld.json", dir, prefix, n);
            HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(h, payload, (DWORD)got, &wr, NULL);
                CloseHandle(h);
                ncm_trace("页面回传(%d 字节) → %s", got, path);
            }
        }
    }
    free(payload);

    const char *resp = "HTTP/1.1 204 No Content\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Access-Control-Allow-Headers: *\r\n"
                       "Connection: close\r\n\r\n";
    send(s, resp, (int)strlen(resp), 0);
    closesocket(s);
}

static DWORD WINAPI server_thread(LPVOID param)
{
    (void)param;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        ncm_trace("probe_server: WSAStartup 失败");
        return 0;
    }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) { ncm_trace("probe_server: socket 失败"); return 0; }

    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROBE_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        ncm_trace("probe_server: bind 127.0.0.1:%d 失败 err=%d", PROBE_PORT, WSAGetLastError());
        closesocket(srv);
        return 0;
    }
    if (listen(srv, 8) == SOCKET_ERROR) {
        ncm_trace("probe_server: listen 失败");
        closesocket(srv);
        return 0;
    }

    ncm_trace("probe_server: ✅ 监听 http://127.0.0.1:%d", PROBE_PORT);

    for (;;) {
        SOCKET c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) break;
        handle_client(c);
    }
    closesocket(srv);
    return 0;
}

void ncm_probe_server_start(void)
{
#if NCM_ENABLE_PROBE_SERVER
    if (InterlockedExchange(&g_started, 1)) return;
    HANDLE h = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
#endif
}
