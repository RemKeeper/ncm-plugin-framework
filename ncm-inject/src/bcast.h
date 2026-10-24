/**
 * bcast.h — 局域网投送（SSE / 封面）
 */
#ifndef NCM_BCAST_H
#define NCM_BCAST_H

#if defined(__cplusplus)
extern "C" {
#endif

/** 启动局域网投送服务（幂等）。端口/绑定地址读 broadcast.json。 */
void ncm_bcast_start(void);

/** 实际监听端口（0 = 未启动）。 */
int  ncm_bcast_port(void);

/** 通知 SSE 客户端：插件变更，请热重载。 */
void ncm_bcast_notify_reload(void);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_BCAST_H */
