/**
 * mitm.h — 资源 MITM（改写 orpheus:// 页面 HTML 以注入载荷）
 */
#ifndef NCM_MITM_H
#define NCM_MITM_H

#if defined(__cplusplus)
extern "C" {
#endif

/** cef_register_scheme_handler_factory 的替换实现。
 *  按 3 参接收（不同 CEF 版本参数个数不同），内部自行挑出 factory。 */
int ncm_mitm_register_hook(const void *scheme_name, void *f2, void *f3);

/** 真实调用次数（给布局转储用，避免依赖日志通道）。 */
long ncm_mitm_create_calls(void);
long ncm_mitm_read_calls(void);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_MITM_H */
