/**
 * dllmain.c — msimg32.dll 代理 + 注入线程启动
 *
 * 这一层刻意保持极薄（docs/03 §3.1 的设计原则）：
 *   它只做「把自己挂上去 + 派一个线程」，所有业务逻辑在 inject.c / payload JS 里。
 *   这样入口层几乎不需要随目标版本变化，也最容易在出问题时整体摘掉。
 *
 * 三条用真机崩溃换来的铁律（docs/01 §1.6.1）必须遵守：
 *   1. DllMain 里不能干活（loader lock 还握着）→ 只 CreateThread
 *   2. 线程里先用 _beginthreadex 而不是 CreateThread（要调 CRT）
 *   3. 线程开头先等一会，让加载器把锁放掉
 */
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>

#include "util.h"
#include "inject.h"
#include "overlay.h"
#include "resdump.h"
#include "probe_server.h"
#include "bcast.h"
#include "pet_hook.h"
#include "devtools.h"
#include "plugins.h"
#include "host.h"

#pragma comment(lib, "version.lib")

/* msimg32 的 5 个导出（转发到系统真身）由 proxy_msimg32.asm / .def 提供，
 * 槽位接在 proxy.c 里。 */
extern int ncm_proxy_resolve(void);

static HMODULE g_hinst = NULL;
static HANDLE  g_thread = NULL;
static DWORD   g_main_tid = 0;

/* ══════════════════════════════════════════════════════════════════════════
 * 角色 / 路径
 * ══════════════════════════════════════════════════════════════════════════ */

static const char *detect_role(void)
{
    const wchar_t *cl = GetCommandLineW();
    if (!cl) return "Main";
    if (wcsstr(cl, L"--type=renderer"))     return "Renderer";
    if (wcsstr(cl, L"--type=gpu-process"))  return "GpuProcess";
    if (wcsstr(cl, L"--type=utility"))      return "Utility";
    if (wcsstr(cl, L"--type="))             return "Other";
    return "Main";
}

static void get_file_version(const wchar_t *path, char *out, int out_size)
{
    out[0] = 0;
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
    if (!sz) return;
    void *buf = malloc(sz);
    if (!buf) return;
    if (GetFileVersionInfoW(path, 0, sz, buf)) {
        VS_FIXEDFILEINFO *ff = NULL;
        UINT len = 0;
        if (VerQueryValueW(buf, L"\\", (void **)&ff, &len) && ff) {
            _snprintf_s(out, out_size, _TRUNCATE, "%u.%u.%u.%u",
                        (unsigned)HIWORD(ff->dwFileVersionMS), (unsigned)LOWORD(ff->dwFileVersionMS),
                        (unsigned)HIWORD(ff->dwFileVersionLS), (unsigned)LOWORD(ff->dwFileVersionLS));
        }
    }
    free(buf);
}

static int dir_exists(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* ══════════════════════════════════════════════════════════════════════════
 * 工作线程
 * ══════════════════════════════════════════════════════════════════════════ */

static unsigned __stdcall worker(void *arg)
{
    (void)arg;

    /* ══ 第 0 步（必须最先做）：抢在 cef_execute_process 之前把 IAT 换掉 ══
     *
     * 子进程（renderer）在 cloudmusic.dll 加载完之后**几乎立刻**就调
     * cef_execute_process，而走完整流程（开日志→枚举模块→扫插件）要 200ms，
     * 根本抢不到。第一次实机跑的结果就是这样：IAT 补丁装上了，但函数早被调过 ——
     * 日志里 补丁=2 却没有一条 hook 记录（docs/07 §7.2.1）。
     *
     * 所以这里不放日志、不扫插件，只做一件事：等加载器把真身地址写进 IAT 槽位，
     * 然后精确地把它换成我们的函数。
     *
     * Sleep(3) 而不是 probe 时代的 50：那时是为了躲 loader lock，
     * 而 arm_early 本身只做 GetModuleHandle/GetProcAddress/VirtualProtect，
     * 撞上锁只会阻塞（不会死锁），正好帮我们串到导入表写完之后。 */
    Sleep(3);
    int armed = ncm_inject_arm_early(8000);

    const char *role = detect_role();

    /* ── 路径 ── */
    wchar_t local[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local)))
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);

    wchar_t profile[MAX_PATH], plugins[MAX_PATH], data_root[MAX_PATH];
    _snwprintf_s(profile, MAX_PATH, _TRUNCATE, L"%s\\NCMPlugin", local);
    _snwprintf_s(plugins, MAX_PATH, _TRUNCATE, L"%s\\plugins", profile);
    _snwprintf_s(data_root, MAX_PATH, _TRUNCATE, L"%s\\NetEase\\CloudMusic", local);

    CreateDirectoryW(profile, NULL);

    /* payload 目录：优先用 DLL 旁边的，其次用 profile 下的 */
    wchar_t dll_path[MAX_PATH] = {0}, dll_dir[MAX_PATH] = {0}, payload[MAX_PATH] = {0};
    GetModuleFileNameW(g_hinst, dll_path, MAX_PATH);
    wcsncpy_s(dll_dir, MAX_PATH, dll_path, _TRUNCATE);
    wchar_t *slash = wcsrchr(dll_dir, L'\\');
    if (slash) *slash = 0;

    _snwprintf_s(payload, MAX_PATH, _TRUNCATE, L"%s\\payload", dll_dir);
    if (!dir_exists(payload)) {
        _snwprintf_s(payload, MAX_PATH, _TRUNCATE, L"%s\\payload", profile);
        if (!dir_exists(payload)) {
            wcsncpy_s(payload, MAX_PATH, dll_dir, _TRUNCATE);  /* 最后的兜底 */
        }
    }

    /* ── 日志 ── */
    wchar_t logp[MAX_PATH];
    _snwprintf_s(logp, MAX_PATH, _TRUNCATE, L"%s\\inject-%hs-%lu.log",
                 profile, role, GetCurrentProcessId());
    ncm_log_open(logp);

    /* ── 基础信息 ── */
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    char exe8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, exe, -1, exe8, sizeof(exe8), NULL, NULL);

    char ver[64] = {0};
    get_file_version(exe, ver, sizeof(ver));

    char cl8[2048] = {0};
    WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, cl8, sizeof(cl8), NULL, NULL);

    ncm_mods_refresh();
    ncm_mod ncm_mod_self;
    memset(&ncm_mod_self, 0, sizeof(ncm_mod_self));
    {
        ncm_mod *m = ncm_mods_find_by_addr((uintptr_t)g_hinst);
        if (m) ncm_mod_self = *m;
    }

    ncm_log("========================================================");
    ncm_log("ncm-inject (msimg32 proxy) 已载入");
    ncm_log("  role        = %s", role);
    ncm_log("  pid/tid     = %lu / %lu", GetCurrentProcessId(), GetCurrentThreadId());
    ncm_log("  self base   = %p   (第 %d 个模块，共 %d 个)",
            (void *)g_hinst, 0, ncm_mods_count());
    ncm_log("  self path   = %ls", ncm_mod_self.path);
    ncm_log("  exe         = %s", exe8);
    ncm_log("  ncm version = %s", ver);
    ncm_log("  cmdline     = %s", cl8);
    ncm_log("  profile     = %ls", profile);
    ncm_log("  plugins     = %ls", plugins);
    ncm_log("  payload     = %ls", payload);
    ncm_log("  logfile     = %ls", logp);
    ncm_log("  libcef      = %p    cloudmusic = %p",
            (void *)GetModuleHandleA("libcef.dll"),
            (void *)GetModuleHandleA("cloudmusic.dll"));

    if (dir_exists(data_root)) ncm_log_w(L"  data root   = %ls", data_root);
    else                       ncm_log_w(L"  data root   = %ls (不存在)", data_root);

    /* ── 插件扫描 ── */
    ncm_plugins_set_meta("0.2.0", role, ver, profile, plugins, data_root);
    ncm_plugins_set_payload_dir(payload);
    ncm_plugins_scan(plugins);

    /* ── 补丁状态 ── (所有角色都要装：get_render_process_handler 是在子进程里被调的) */
    int dm = ncm_inject_dllmain_armed();
    if (dm > 0) {
        ncm_log("✅ DllMain 内抢装完成：%d 个补丁（libcef 是导入描述符 #12、MSIMG32 是 #33，"
                "加载器先回填 libcef 的 IAT，所以这个时刻替换是零竞争的）", dm);
    }    if (armed) {
        ncm_log("worker 抢装成功：%d 个补丁，耗时 %d ms", armed, ncm_inject_armed_ms());
    } else if (dm <= 0) {
        ncm_log("⚠️ 抢装没成功（DllMain=%d，worker 耗时 %d ms）；改走常规安装，"
                "子进程里可能已经错过入口", dm, ncm_inject_armed_ms());
        ncm_inject_install();
    }

    /* 防加载器后写 */
    int vf = ncm_inject_verify(2500);
    if (vf) ncm_log("verify: 补写了 %d 次", vf);

    ncm_log("补丁安装结束：%d 个，等待 CEF 回调……", ncm_inject_hook_count());
    ncm_log("========================================================");

    /* ── 保活 + 插件目录变更监测（下一段载荷重新组装时自然生效） ── */
    long long last_mtime = ncm_plugins_probe_mtime();
    int tick = 0;          /* 100ms 拍 */
    int overlay_started = 0;
    int resdump_installed = 0;
    int badge_done = 0;    /* 宿主窗口标题是否已打标记 */
    for (;;) {
        /* ★ 排空间隔 100ms 而不是 2s：页面 JS 若在某个上下文里立刻崩掉，
         *   只有足够快的落盘才能把它崩溃前最后一条面包屑留下来（排查用）。 */
        Sleep(100);
        tick++;

        /* ★ 资源链 hook 必须尽早装：页面可能在角色判定之前就加载完了。 */
        /* 页面回传通道：尽早起，页面加载后就能上报 */
        ncm_probe_server_start();

        if (!resdump_installed) {
            resdump_installed = 1;
            ncm_resdump_install();
        }

        /* ★ 页面 JS 投递过来的日志在这里落盘。
         *   绝不能在 V8 线程里写日志：实测那样会让应用在 8 秒内整体退出
         *   （docs/07 §7.2.5）。 */
        ncm_host_drain();

        if (tick % 20 == 0) {          /* 每 2s */
            /* ★ P2「可见效果」：只有主进程有 UI，给它打上注入标记。
             *   这不是装饰 —— 它是**可机器验证**的证据：
             *   `(Get-Process cloudmusic).MainWindowTitle` 能直接读到这行字，
             *   证明我们的代码不只是载入，还真的在跑并能操作宿主窗口。
             *   docs/07 §7.11。 */
            /* ★ 每 2 s 维护一次：NCM 自己会改窗口标题（显示播放信息），
             *   一次追加会被它覆盖掉，所以这里持续补齐，缺了才补、补了才记日志。 */
            if (strcmp(role, "Main") == 0) {
                ncm_bcast_start();
        ncm_pet_hook_start();   /* 桌宠窗口的点击/拖拽旁路 */
                /* 收尾：不再改动宿主窗口标题（原先会追加 " [ncm-inject]"） */
                /* 收尾：不再创建原生浮窗，UI 统一由页面内右上角小角标承担。
                 * 保留代码路径便于排查，开关置 0 即关闭。 */
                if (0 && !overlay_started) {
                    overlay_started = 1;
                    char txt[224];
                    int okc = 0, all = ncm_plugins_count();
                    char names[120] = "";
                    for (int i = 0; i < all; i++) {
                        ncm_plugin *p = ncm_plugins_get(i);
                        if (!p || !p->ok) continue;
                        okc++;
                        if (names[0]) strncat_s(names, sizeof(names), ", ", _TRUNCATE);
                        strncat_s(names, sizeof(names), p->name[0] ? p->name : p->slug, _TRUNCATE);
                    }
                    _snprintf_s(txt, sizeof(txt), _TRUNCATE,
                                "NCM Plugin Host · %d/%d 插件：%s", okc, all, names[0] ? names : "(无)");
                    ncm_overlay_start(txt);
                }
            }

            /* 浏览器进程：frame 就绪后再注入页面（create 时刻还是 NULL） */
            ncm_inject_page_try();

            long long now = ncm_plugins_probe_mtime();
            if (now != last_mtime) {
                long long old_mtime = last_mtime;
                last_mtime = now;
                ncm_log("检测到插件目录变更（mtime %lld → %lld），重扫", old_mtime, now);
                ncm_bcast_notify_reload();
                ncm_plugins_scan(plugins);
                ncm_inject_page_reset();   /* 热重载生效侧：重新投递页面载荷 */
            }
        }

        if (tick % 300 == 0) {         /* 每 30s */
            ncm_inject_dump_layout();
            ncm_log("心跳 role=%s uptime≈%ds", role, tick / 10);
        }
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * DllMain
 * ══════════════════════════════════════════════════════════════════════════ */

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = hinst;
        DisableThreadLibraryCalls(hinst);
        /* ★ 转发槽位必须在 DllMain 里就接上（宿主静态导入 MSIMG32，
         *   加载器随时可能调到我们的导出）。这是 DllMain 里唯一该做且必须做的事。 */
        ncm_proxy_resolve();
        /* ★★ 抢装：唯一的零竞争时机。放在这里是因为此刻加载器已经按导入描述符
         *    的顺序把 libcef 那批 IAT 槽位填好了（libcef 是 #12，MSIMG32 是 #33），
         *    而主线程还没来得及调 cef_execute_process。
         *    起线程之后再抢就已经晚了 —— 见 ncm_inject_arm_in_dllmain 的注释。
         *    这里只读 PEB + 改指针，不开日志、不调加载器、不碰堆。 */
        ncm_inject_arm_in_dllmain();
        /* ★ 铁律 1 + 2：其他活一律不在这里干，只起线程，且必须用 _beginthreadex */
        g_thread = (HANDLE)_beginthreadex(NULL, 0, worker, NULL, 0, &g_main_tid);
        if (!g_thread) {
            /* 起不了线程也要尽量记一笔（此时没有任何设施可用，只能走调试输出） */
            OutputDebugStringA("[ncm-inject] _beginthreadex failed\n");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        /* 注意：`reserved != NULL` 表示进程正在退出（这是**唯一**能区分
         * "被 FreeLibrary 卸载" 和 "进程正常退出" 的信号 —— 见 docs/05 §5.3.1.1）。
         * 观测侧就是靠这一行判断有没有被主动卸载。 */
        ncm_log("DLL_PROCESS_DETACH (%s), reserved=%p, tid=%lu",
                reserved ? "进程退出" : "被显式卸载/FreeLibrary",
                reserved, GetCurrentThreadId());
        ncm_log_close();
    }
    return TRUE;
}
