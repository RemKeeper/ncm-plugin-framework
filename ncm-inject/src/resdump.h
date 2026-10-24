/**
 * resdump.h — 资源链取证（LoadByUrl hook）
 */
#ifndef NCM_RESDUMP_H
#define NCM_RESDUMP_H

#if defined(__cplusplus)
extern "C" {
#endif

/** 安装 LoadByUrl 的 dump hook（幂等）。 */
void ncm_resdump_install(void);

/** 安装 is-file 重定向（配合 patched 文件）。 */
void ncm_resdump_install_redirect(void);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_RESDUMP_H */
