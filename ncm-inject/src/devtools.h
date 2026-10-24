/**
 * devtools.h — CEF 远程调试端口
 */
#ifndef NCM_DEVTOOLS_H
#define NCM_DEVTOOLS_H

#if defined(__cplusplus)
extern "C" {
#endif

/** 安装 DevTools 支持（幂等）。必须在 CEF 初始化之前调用。 */
void ncm_devtools_install(void);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_DEVTOOLS_H */
