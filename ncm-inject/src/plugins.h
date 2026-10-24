/**
 * plugins.h — 插件目录扫描 + 注入载荷（bundle）组装
 */
#ifndef NCM_PLUGINS_H
#define NCM_PLUGINS_H

#include <windows.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct ncm_plugin {
    char  slug[128];
    char  dir_utf8[512];
    char  script_file[128];

    /* ── 从 manifest.json 抠出来的结构字段（BetterNCM PluginManifest 子集） ──
     * 原生侧只做"够用来做加载决策"的解析，语义完整解析仍留给 JS 侧；
     * 这样即使页面通道暂时不可用，宿主也能正确报告与校验插件。 */
    char  name[128];
    char  version[64];
    char  author[96];
    char  ncm_version_req[64];
    int   manifest_version;      /* 0 = 缺失 */
    int   ncm3_compatible;       /* -1 = 未声明 */
    int   has_injects;           /* manifest 里有非空 injects 段 */
    int   has_hijacks;           /* manifest 里有非空 hijacks 段 */

    char *manifest_json;    /* 原始 manifest.json 文本（JS 侧解析） */
    char *script_src;       /* 入口脚本源码 */
    long long mtime;        /* 入口脚本最后写入时间（100ns） */
    int   ok;               /* 1 = 可加载 */
    char  error[256];       /* ok=0 时的原因 */
} ncm_plugin;

/** 扫描 <pluginsDir>\*\，读 manifest.json 与入口脚本。返回插件数（含失败的）。 */
int  ncm_plugins_scan(const wchar_t *plugins_dir);

int  ncm_plugins_count(void);
ncm_plugin *ncm_plugins_get(int i);

/** 插件目录的最近修改时间，用于热重载轮询（0 = 目录不存在）。 */
long long ncm_plugins_dir_mtime(void);

/** 热重载用：**实时**探测插件目录及所有插件文件的最新修改时间（不查缓存）。 */
long long ncm_plugins_probe_mtime(void);

/** 组装要注入页面的完整 JS：__NCM_HOST__ 数据 + payload 源码。调用方 free()。 */
char *ncm_bundle_build(void);

/** 单独取 __NCM_HOST__ 的 JSON 文本。调用方 free()。 */
char *ncm_host_json(void);

/** 插件数据根目录（%LOCALAPPDATA%\\NCMPlugin\\data）。 */
const wchar_t *ncm_data_root(void);

void ncm_plugins_set_payload_dir(const wchar_t *dir);
void ncm_plugins_set_meta(const char *host_version, const char *process_type,
                          const char *ncm_version, const wchar_t *profile_dir,
                          const wchar_t *plugins_dir, const wchar_t *data_root);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_PLUGINS_H */
