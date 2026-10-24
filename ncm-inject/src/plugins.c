/**
 * plugins.c — 插件目录扫描 + bundle 组装
 *
 * 设计取舍（见 docs/07 §7.4）：
 *   C++ 侧**不做 manifest 语义解析**，只做两件事：
 *     1. 把 manifest.json 的**原文**读出来
 *     2. 把入口脚本（manifest.startup_script，缺省 startup_script.js）的**源码**读出来
 *   然后把两者原样塞进 __NCM_HOST__.plugins[] 交给页面里的 JS。
 *
 *   这样插件规范怎么演进（新增字段、新的生命周期）都**不需要重编 DLL** ——
 *   只改 payload 里的 JS。这是 chromatic「引擎与插件解耦」思路在 NCM 上的落地。
 */
#include "plugins.h"
#include "bcast.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <share.h>
#include <sys/stat.h>

#define MAX_PLUGINS 128

static ncm_plugin g_plugins[MAX_PLUGINS];
static int        g_count = 0;
static long long  g_dir_mtime = 0;

static wchar_t    g_plugins_dir[MAX_PATH] = {0};
static wchar_t    g_payload_dir[MAX_PATH] = {0};
static wchar_t    g_profile_dir[MAX_PATH] = {0};
static wchar_t    g_data_root[MAX_PATH]   = {0};
static char       g_host_version[32] = "0.2.0";
static char       g_process_type[32] = "unknown";
static char       g_ncm_version[64]  = "unknown";

/* ══════════════════════════════════════════════════════════════════════════
 * 小工具：动态字符串 / 文件读取 / JSON 转义
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} sbuf;

static void sb_init(sbuf *b, size_t cap)
{
    b->p = (char *)malloc(cap);
    b->len = 0;
    b->cap = cap;
    if (b->p) b->p[0] = 0;
}

static void sb_reserve(sbuf *b, size_t extra)
{
    if (!b->p) return;
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + extra + 1) nc *= 2;
    char *np = (char *)realloc(b->p, nc);
    if (!np) return;
    b->p = np;
    b->cap = nc;
}

static void sb_addn(sbuf *b, const char *s, size_t n)
{
    if (!b->p) return;
    sb_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_add(sbuf *b, const char *s) { if (s) sb_addn(b, s, strlen(s)); }
static void sb_addc(sbuf *b, char c)        { sb_addn(b, &c, 1); }

/** 把一个 UTF-8/任意字节串转义成 JSON 字符串内容（不含引号）。 */
static void sb_add_json_escaped(sbuf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  sb_add(b, "\\\""); break;
        case '\\': sb_add(b, "\\\\"); break;
        case '\n': sb_add(b, "\\n");  break;
        case '\r': sb_add(b, "\\r");  break;
        case '\t': sb_add(b, "\\t");  break;
        case '\b': sb_add(b, "\\b");  break;
        case '\f': sb_add(b, "\\f");  break;
        default:
            if (c < 0x20) {
                char tmp[8];
                _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "\\u%04X", c);
                sb_add(b, tmp);
            } else {
                /* UTF-8 多字节原样透传（JSON 允许） */
                sb_addc(b, (char)c);
            }
        }
    }
}

/** 读整个文件（UTF-8 字节）。调用方 free()。失败返回 NULL。
 *  ★ 用 _wfsopen + _SH_DENYNO，避免与正在写文件的进程互锁。 */
static char *read_file_utf8(const wchar_t *path, size_t *out_size)
{
    if (out_size) *out_size = 0;
    FILE *f = _wfsopen(path, L"rb", _SH_DENYNO);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    if (out_size) *out_size = got;
    return buf;
}

static void utf8_of(const wchar_t *w, char *out, int out_size)
{
    out[0] = 0;
    if (!w) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, out_size, NULL, NULL);
}

/** 极简 JSON 字段提取：找 "key" 后面第一个字符串值。仅用于取 startup_script。 */
static int json_get_string(const char *json, const char *key, char *out, int out_size)
{
    out[0] = 0;
    if (!json) return 0;
    char pat[96];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = 0;
    return i;
}

/* ══════════════════════════════════════════════════════════════════════════
 * 扫描
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── manifest 结构字段的最小解析 ──────────────────────────────────────────
 * 只做「够做加载决策」的程度：字符串 / 整数 / 布尔 / 段落是否存在。
 * 真正的语义解析（injects/hijacks 的完整结构）仍然留给 JS 侧，
 * 原生侧不解 JSON —— 这是 docs/03 里定下的设计，避免两份解析器不一致。
 */

static int json_get_int(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) return 0;
    char pat[96];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p < '0' || *p > '9') return 0;
    *out = atoi(p);
    return 1;
}

static int json_get_bool(const char *json, const char *key, int *out)
{
    if (!json || !key || !out) return 0;
    char pat[96];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "true", 4) == 0)  { *out = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

/** key 后面跟的是不是「非空的对象/数组」。 */
static int json_key_nonempty_container(const char *json, const char *key)
{
    if (!json || !key) return 0;
    char pat[96];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '{') { const char *q = p + 1; while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++; return *q != '}'; }
    if (*p == '[') { const char *q = p + 1; while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++; return *q != ']'; }
    return 0;
}

/** 极简 semver 比较：-1 / 0 / 1；无法解析时返回 2。 */
static int ver_cmp(const char *a, const char *b)
{
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (!a || !b) return 2;
    if (sscanf_s(a, "%d.%d.%d", &a1, &a2, &a3) < 1) return 2;
    if (sscanf_s(b, "%d.%d.%d", &b1, &b2, &b3) < 1) return 2;
    if (a1 != b1) return a1 < b1 ? -1 : 1;
    if (a2 != b2) return a2 < b2 ? -1 : 1;
    if (a3 != b3) return a3 < b3 ? -1 : 1;
    return 0;
}

/** 支持 BetterNCM 常见的 ">=3.0.0 <4" 形式（空格分隔多个条件，全部满足才算）。 */
static int version_satisfies(const char *req, const char *cur)
{
    if (!req || !req[0]) return 1;              /* 没写要求 = 不限制 */
    if (ver_cmp(cur, cur) == 2) return 1;       /* 当前版本号拿不到，别拦 */

    const char *p = req;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        int op = 0;                              /* 0=>= 1=> 2=<= 3=< 4=== */
        if (p[0] == '>' && p[1] == '=') { op = 0; p += 2; }
        else if (p[0] == '<' && p[1] == '=') { op = 2; p += 2; }
        else if (p[0] == '>') { op = 1; p += 1; }
        else if (p[0] == '<') { op = 3; p += 1; }
        else if (p[0] == '=') { op = 4; p += 1; }
        while (*p == ' ') p++;

        char v[32]; int k = 0;
        while (*p && *p != ' ' && k < (int)sizeof(v) - 1) v[k++] = *p++;
        v[k] = 0;
        if (!k) break;

        int c = ver_cmp(cur, v);
        if (c == 2) continue;
        int ok = (op == 0) ? (c >= 0) : (op == 1) ? (c > 0) : (op == 2) ? (c <= 0) : (op == 3) ? (c < 0) : (c == 0);
        if (!ok) return 0;
    }
    return 1;
}

void ncm_plugins_set_payload_dir(const wchar_t *dir)
{
    wcsncpy_s(g_payload_dir, MAX_PATH, dir ? dir : L"", _TRUNCATE);
}

void ncm_plugins_set_meta(const char *host_version, const char *process_type,
                          const char *ncm_version, const wchar_t *profile_dir,
                          const wchar_t *plugins_dir, const wchar_t *data_root)
{
    if (host_version)  strncpy_s(g_host_version, sizeof(g_host_version), host_version, _TRUNCATE);
    if (process_type)  strncpy_s(g_process_type, sizeof(g_process_type), process_type, _TRUNCATE);
    if (ncm_version)   strncpy_s(g_ncm_version, sizeof(g_ncm_version), ncm_version, _TRUNCATE);
    if (profile_dir)   wcsncpy_s(g_profile_dir, MAX_PATH, profile_dir, _TRUNCATE);
    if (plugins_dir)   wcsncpy_s(g_plugins_dir, MAX_PATH, plugins_dir, _TRUNCATE);
    if (data_root)     wcsncpy_s(g_data_root, MAX_PATH, data_root, _TRUNCATE);
}

long long ncm_plugins_dir_mtime(void) { return g_dir_mtime; }

/* ★ 热重载用的**实时**探测：不能被缓存。
 *
 * 之前心跳线程直接调 ncm_plugins_dir_mtime()，而它返回的是上次扫描时缓存的
 * g_dir_mtime —— 常量，所以「目录变更」永远检测不到（实测：运行中改脚本，
 * 日志里一条变更记录都没有）。这里每次重新 stat 一遍目录 + 每个插件的所有文件，
 * 取最大值（单位 100ns，和 g_dir_mtime 的秒级值混用没关系，只做大小比较）。 */
long long ncm_plugins_probe_mtime(void)
{
    if (!g_plugins_dir[0]) return 0;

    long long best = 0;
    struct _stati64 st;
    if (_wstati64(g_plugins_dir, &st) == 0) best = (long long)st.st_mtime * 10000000LL;

    wchar_t pat[MAX_PATH + 8];
    _snwprintf_s(pat, MAX_PATH + 8, _TRUNCATE, L"%s\\*", g_plugins_dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return best;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        wchar_t sub[MAX_PATH];
        _snwprintf_s(sub, MAX_PATH, _TRUNCATE, L"%s\\%s", g_plugins_dir, fd.cFileName);
        /* ★ 目录 mtime 不会因为"改了目录里的文件"而变化（NTFS 行为），
         *   所以必须把插件目录内**文件**的 mtime 也算进来，
         *   否则热重载检测不到"只改了 startup_script.js"这种情况。 */
        {
            wchar_t fpat[MAX_PATH + 8];
            _snwprintf_s(fpat, MAX_PATH + 8, _TRUNCATE, L"%s\\*", sub);
            WIN32_FIND_DATAW ffd;
            HANDLE fh = FindFirstFileW(fpat, &ffd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    long long ft = ((long long)ffd.ftLastWriteTime.dwHighDateTime << 32)
                                 | (long long)ffd.ftLastWriteTime.dwLowDateTime;
                    if (ft > best) best = ft;
                } while (FindNextFileW(fh, &ffd));
                FindClose(fh);
            }
        }
        if (_wstati64(sub, &st) == 0) {
            long long t = (long long)st.st_mtime * 10000000LL;
            if (t > best) best = t;
        }

        wchar_t spat[MAX_PATH + 8];
        _snwprintf_s(spat, MAX_PATH + 8, _TRUNCATE, L"%s\\*", sub);
        WIN32_FIND_DATAW sfd;
        HANDLE sh = FindFirstFileW(spat, &sfd);
        if (sh != INVALID_HANDLE_VALUE) {
            do {
                if (sfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                long long t = ((long long)sfd.ftLastWriteTime.dwHighDateTime << 32)
                            | sfd.ftLastWriteTime.dwLowDateTime;
                if (t > best) best = t;
            } while (FindNextFileW(sh, &sfd));
            FindClose(sh);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    return best;
}

int ncm_plugins_count(void) { return g_count; }
ncm_plugin *ncm_plugins_get(int i)
{
    return (i >= 0 && i < g_count) ? &g_plugins[i] : NULL;
}

int ncm_plugins_scan(const wchar_t *plugins_dir)
{
    /* 清掉上一轮 */
    for (int i = 0; i < g_count; i++) {
        free(g_plugins[i].manifest_json);
        free(g_plugins[i].script_src);
        g_plugins[i].manifest_json = NULL;
        g_plugins[i].script_src = NULL;
    }
    g_count = 0;
    g_dir_mtime = 0;

    if (plugins_dir && plugins_dir[0])
        wcsncpy_s(g_plugins_dir, MAX_PATH, plugins_dir, _TRUNCATE);

    if (!g_plugins_dir[0]) return 0;

    wchar_t pat[MAX_PATH + 8];
    _snwprintf_s(pat, MAX_PATH + 8, _TRUNCATE, L"%s\\*", g_plugins_dir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        /* ★ 用 ncm_log_w：宽→UTF-8 由它内部做（宽 printf + %ls 是安全的）。
         *   反过来用窄 ncm_log("...%ls") 输出非 ASCII 宽串会整行截断（docs/01 §1.6.2 第 6 条）。 */
        ncm_log_w(L"插件目录不存在或为空: %ls", g_plugins_dir);
        return 0;
    }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;             /* . / .. / 隐藏 */
        if (g_count >= MAX_PLUGINS) break;

        ncm_plugin *pl = &g_plugins[g_count];
        memset(pl, 0, sizeof(*pl));
        g_count++;

        /* slug = 目录名 */
        utf8_of(fd.cFileName, pl->slug, sizeof(pl->slug));

        wchar_t dir[MAX_PATH];
        _snwprintf_s(dir, MAX_PATH, _TRUNCATE, L"%s\\%s", g_plugins_dir, fd.cFileName);
        utf8_of(dir, pl->dir_utf8, sizeof(pl->dir_utf8));

        /* manifest.json（原文） */
        wchar_t mp[MAX_PATH];
        _snwprintf_s(mp, MAX_PATH, _TRUNCATE, L"%s\\manifest.json", dir);
        pl->manifest_json = read_file_utf8(mp, NULL);
        if (!pl->manifest_json) {
            pl->ok = 0;
            strncpy_s(pl->error, sizeof(pl->error), "缺少 manifest.json", _TRUNCATE);
            ncm_log("插件 %s: 缺少 manifest.json", pl->slug);
            continue;
        }

        /* ── manifest 结构字段 ── */
        if (!json_get_int(pl->manifest_json, "manifest_version", &pl->manifest_version))
            pl->manifest_version = 0;
        json_get_string(pl->manifest_json, "name", pl->name, sizeof(pl->name));
        json_get_string(pl->manifest_json, "version", pl->version, sizeof(pl->version));
        json_get_string(pl->manifest_json, "author", pl->author, sizeof(pl->author));
        json_get_string(pl->manifest_json, "ncm_version_req",
                        pl->ncm_version_req, sizeof(pl->ncm_version_req));
        if (!json_get_bool(pl->manifest_json, "ncm3Compatible", &pl->ncm3_compatible))
            pl->ncm3_compatible = -1;
        pl->has_injects = json_key_nonempty_container(pl->manifest_json, "injects");
        pl->has_hijacks = json_key_nonempty_container(pl->manifest_json, "hijacks");

        /* 入口脚本：manifest.startup_script（缺省 startup_script.js） */
        if (!json_get_string(pl->manifest_json, "startup_script",
                             pl->script_file, sizeof(pl->script_file)))
            strncpy_s(pl->script_file, sizeof(pl->script_file), "startup_script.js", _TRUNCATE);

        wchar_t sp[MAX_PATH];
        _snwprintf_s(sp, MAX_PATH, _TRUNCATE, L"%s\\%hs", dir, pl->script_file);

        struct _stati64 st;
        pl->mtime = 0;
        /* ★ 以「脚本 + manifest + 目录」三者的最大 mtime 作为插件版本标记。
         *   只取目录 mtime 的话，"只改了 startup_script.js" 检测不到（NTFS 不会更新父目录 mtime），
         *   热重载就会失效。 */
        if (_wstati64(sp, &st) == 0) pl->mtime = (long long)st.st_mtime * 10000000LL;
        {
            wchar_t mp2[MAX_PATH];
            _snwprintf_s(mp2, MAX_PATH, _TRUNCATE, L"%s\\manifest.json", dir);
            struct _stati64 st2;
            if (_wstati64(mp2, &st2) == 0) {
                long long t2 = (long long)st2.st_mtime * 10000000LL;
                if (t2 > pl->mtime) pl->mtime = t2;
            }
        }
        {
            struct _stati64 st3;
            if (_wstati64(dir, &st3) == 0) {
                long long t3 = (long long)st3.st_mtime * 10000000LL;
                if (t3 > pl->mtime) pl->mtime = t3;
            }
        }
        if (st.st_mtime > g_dir_mtime) g_dir_mtime = st.st_mtime;

        size_t ssz = 0;
        pl->script_src = read_file_utf8(sp, &ssz);
        if (!pl->script_src) {
            pl->ok = 0;
            _snprintf_s(pl->error, sizeof(pl->error), _TRUNCATE,
                        "读不到入口脚本 %s", pl->script_file);
            ncm_log("插件 %s: 读不到入口脚本 %s", pl->slug, pl->script_file);
            continue;
        }

        /* ── 加载决策（与 BetterNCM 判据对齐：manifest_version==2 + 版本区间） ── */
        if (pl->manifest_version != 2) {
            pl->ok = 0;
            _snprintf_s(pl->error, sizeof(pl->error), _TRUNCATE,
                        "manifest_version=%d（要求 2）", pl->manifest_version);
            ncm_log("插件 %s: 拒绝加载 —— %s", pl->slug, pl->error);
            continue;
        }
        if (!version_satisfies(pl->ncm_version_req, g_ncm_version)) {
            pl->ok = 0;
            _snprintf_s(pl->error, sizeof(pl->error), _TRUNCATE,
                        "版本不满足 \"%s\"（当前 %s）", pl->ncm_version_req, g_ncm_version);
            ncm_log("插件 %s: 拒绝加载 —— %s", pl->slug, pl->error);
            continue;
        }

        pl->ok = 1;
        ncm_log("插件 %s: ok  name=\"%s\" v%s  entry=%s  ncm_req=\"%s\"  ncm3=%d  "
                "injects=%d hijacks=%d  manifest=%zuB script=%zuB",
                pl->slug, pl->name[0] ? pl->name : pl->slug, pl->version,
                pl->script_file, pl->ncm_version_req,
                pl->ncm3_compatible, pl->has_injects, pl->has_hijacks,
                strlen(pl->manifest_json), ssz);
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    /* 目录本身的修改时间（用于热重载轮询） */
    struct _stati64 dst;
    if (_wstati64(g_plugins_dir, &dst) == 0 && dst.st_mtime > g_dir_mtime)
        g_dir_mtime = dst.st_mtime;

    ncm_log("插件扫描完成：%d 个", g_count);
    return g_count;
}

/* ══════════════════════════════════════════════════════════════════════════
 * __NCM_HOST__ JSON
 * ══════════════════════════════════════════════════════════════════════════ */

char *ncm_host_json(void)
{
    sbuf b;
    sb_init(&b, 8192);
    if (!b.p) return NULL;

    char profile8[MAX_PATH * 3], plugins8[MAX_PATH * 3], data8[MAX_PATH * 3], payload8[MAX_PATH * 3];
    utf8_of(g_profile_dir, profile8, sizeof(profile8));
    utf8_of(g_plugins_dir, plugins8, sizeof(plugins8));
    utf8_of(g_data_root,   data8,    sizeof(data8));
    utf8_of(g_payload_dir, payload8, sizeof(payload8));

    sb_add(&b, "{\"hostVersion\":\"");
    sb_add_json_escaped(&b, g_host_version, strlen(g_host_version));
    sb_add(&b, "\",\"processType\":\"");
    sb_add_json_escaped(&b, g_process_type, strlen(g_process_type));
    sb_add(&b, "\",\"ncmVersion\":\"");
    sb_add_json_escaped(&b, g_ncm_version, strlen(g_ncm_version));
    sb_add(&b, "\",\"profileDir\":\"");
    sb_add_json_escaped(&b, profile8, strlen(profile8));
    sb_add(&b, "\",\"pluginsDir\":\"");
    sb_add_json_escaped(&b, plugins8, strlen(plugins8));
    sb_add(&b, "\",\"dataRoot\":\"");
    sb_add_json_escaped(&b, data8, strlen(data8));
    sb_add(&b, "\",\"payloadDir\":\"");
    sb_add_json_escaped(&b, payload8, strlen(payload8));

    {
        char bp[16];
        _snprintf_s(bp, sizeof(bp), _TRUNCATE, "%d", ncm_bcast_port());
        sb_add(&b, "\",\"injection\":\"iat-hook/on_load_end\",\"bcastPort\":");
        sb_add(&b, bp);
        sb_add(&b, ",\"plugins\":[");
    }
    for (int i = 0; i < g_count; i++) {
        ncm_plugin *pl = &g_plugins[i];
        if (i) sb_addc(&b, ',');
        sb_add(&b, "{\"slug\":\"");
        sb_add_json_escaped(&b, pl->slug, strlen(pl->slug));
        sb_add(&b, "\",\"dir\":\"");
        sb_add_json_escaped(&b, pl->dir_utf8, strlen(pl->dir_utf8));
        sb_add(&b, "\",\"entryFile\":\"");
        sb_add_json_escaped(&b, pl->script_file, strlen(pl->script_file));
        sb_add(&b, "\",\"mtime\":");
        { char t[32]; _snprintf_s(t, sizeof(t), _TRUNCATE, "%lld", pl->mtime); sb_add(&b, t); }
        sb_add(&b, ",\"error\":\"");
        sb_add_json_escaped(&b, pl->error, strlen(pl->error));
        sb_add(&b, "\",\"manifest\":");
        /* manifest 是**原文**，直接当 JSON 值嵌入；解析失败由 JS 侧兜 */
        if (pl->manifest_json) sb_add(&b, pl->manifest_json);
        else                   sb_add(&b, "null");
        sb_add(&b, ",\"script\":\"");
        if (pl->script_src) sb_add_json_escaped(&b, pl->script_src, strlen(pl->script_src));
        sb_add(&b, "\"}");
    }
    sb_add(&b, "]}");
    return b.p;
}

char *ncm_bundle_build(void)
{
    /* 1) 页面载荷源码（payloadDir 下的 ncm-page.js） */
    char *page_src = NULL;
    if (g_payload_dir[0]) {
        wchar_t f[MAX_PATH];
        _snwprintf_s(f, MAX_PATH, _TRUNCATE, L"%s\\ncm-page.js", g_payload_dir);
        page_src = read_file_utf8(f, NULL);
        if (!page_src) ncm_log_w(L"payload 缺失: %ls", f);
    }

    char *host = ncm_host_json();
    if (!host) return NULL;

    sbuf b;
    sb_init(&b, 32768);
    if (!b.p) { free(host); free(page_src); return NULL; }

    /* 用一个 IIFE 把宿主数据挂到 window 上；再把载荷源码整体放进同一段脚本。
     * 之所以合成一段而不是两次 execute_java_script：
     *   载荷里要立刻读 __NCM_HOST__，分两次会在"谁先跑"上引入时序不确定性。 */
    sb_add(&b, "(function(){\n");
    sb_add(&b, "  try {\n");
    sb_add(&b, "    window.__NCM_HOST__ = ");
    sb_add(&b, host);
    sb_add(&b, ";\n");
    sb_add(&b, "    window.__NCM_HOST__.injectedAt = new Date().toISOString();\n");
    sb_add(&b, "  } catch(e) { console.error('[ncm] host json failed', e); }\n");
    if (page_src) {
        sb_add(&b, "  /* ==== payload/ncm-page.js ==== */\n");
        sb_add(&b, page_src);
        sb_add(&b, "\n");
    } else {
        sb_add(&b, "  console.warn('[ncm] payload/ncm-page.js 没读到');\n");
    }
    sb_add(&b, "})();\n");

    free(host);
    free(page_src);
    return b.p;
}

const wchar_t *ncm_data_root(void) { return g_data_root; }
