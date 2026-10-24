/**
 * host.h — 页面 JS ⇄ 本 DLL 的原生桥
 *
 * 为什么走"注册自己的 CEF 扩展"这条路：
 *   实机实测（docs/07 §7.2.4）：渲染进程里
 *     cef_execute_process   被调 1 次（我们 hook 到了，app 也换了）
 *     但 CEF 向 app 要 get_render_process_handler 的次数 = 0
 *     cef_register_extension               被调 1 次（宿主自己的 JS 桥 "v8/channel"）
 *     cef_v8context_get_current_context    被调 1039 次
 *   ⇒ 在渲染进程里，app 那条路是死的；**扩展**才是页面级注入的正式载体。
 *
 * 我们注册一个完全由自己实现的 cef_v8handler_t：
 *   - 结构体布局由我们决定（base + 1 个 execute），不需要猜 libcef 的成员顺序
 *   - JS 侧 `native function __ncmHostCall()` 由 CEF 绑定到我们的 execute
 *   - 于是页面里拿到了直通 DLL 的同步调用，且**可自证**：JS 调 alive/log，
 *     DLL 立刻在自己日志里写一行（这是端到端最硬的证据）
 */
#ifndef NCM_HOST_H
#define NCM_HOST_H

#if defined(__cplusplus)
extern "C" {
#endif

/** 注册我们自己的扩展（只能调一次）。成功返回 1。 */
int ncm_host_register(void);

int ncm_host_calls(void);        /* JS→原生 调用次数 */
int ncm_host_ok(void);           /* 其中成功处理的次数 */
int ncm_host_string_idx(void);   /* 自检出来的 get_string_value 槽位（-1=没探到） */
int ncm_host_registered(void);

/** 把 V8 线程投递过来的日志落盘（只在 worker 心跳线程调）。返回写入条数。 */
int ncm_host_drain(void);

/** 从任意 CEF 线程（含 V8 线程）安全打点：同步写 trace 文件 + 排队进正式日志。 */
void ncm_trace(const char *fmt, ...);

/** 把 cef_v8value_t* 当字符串读出来（复用自检过的 get_string_value 槽位）。 */
int ncm_host_value_to_utf8(void *v, char *out, int out_size);

/** 创建 JS 字符串值（cef_v8value_create_string），用于往页面 global 写属性。 */
void *ncm_create_js_string(const char *utf8);

/** 惰性初始化：libcef 导出 + get_string_value 槽位自检。 */
int ncm_host_ensure(void);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_HOST_H */
