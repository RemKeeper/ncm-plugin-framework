/**
 * overlay.h — 原生浮层（Main 进程可见的插件宿主标记）
 */
#ifndef NCM_OVERLAY_H
#define NCM_OVERLAY_H

#if defined(__cplusplus)
extern "C" {
#endif

void ncm_overlay_start(const char *text);
void ncm_overlay_set_text(const char *text);
void ncm_overlay_stop(void);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_OVERLAY_H */
