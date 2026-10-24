/**
 * cef_capi.h — 自写的 CEF 91 C-API 片段（只含我们要用到的部分）
 *
 * 为什么不直接用官方头文件：
 *   NCM 安装目录不带 SDK 头；本仓库也不引入 155MB 的 libcef 分发。
 *   我们只需要 4 个结构体（base / string / client / load_handler）与 frame 的两个方法，
 *   所以手写一份最小声明，并且**把"布局可能不对"这件事显式化**（见 cef_layout.h）。
 *
 * 关键事实（决定了整个注入层怎么做）：
 *   1) libcef 的接口方法**不导出**，只能在结构体里按偏移访问
 *   2) 但 cloudmusic.dll 是**通过 IAT 静态导入**这 129 个 cef_* 自由函数的
 *      → 所以「进入 CEF」这一步可以用 IAT 打补丁完成，不需要 inline hook
 *   3) cef_client_t / cef_frame_t / cef_v8context_t 的实例由 libcef 或宿主构造，
 *      回调指针指向某个已加载模块 → 可以在运行时"扫描 + 校验"出数组起始偏移
 */
#ifndef NCM_CEF_CAPI_H
#define NCM_CEF_CAPI_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/* ── 字符串 ──────────────────────────────────────────────────────────────── */
/* cef_string_utf16_t = { char16_t* str; size_t length; void (*dtor)(char16_t*); } */
typedef struct ncm_cef_string_utf16 {
    uint16_t *str;
    size_t    length;
    void    (*dtor)(uint16_t *str);
} ncm_cef_string_utf16;

typedef ncm_cef_string_utf16  cef_string_t;
typedef ncm_cef_string_utf16 *cef_string_userfree_t;

/* ── 基类：引用计数 ───────────────────────────────────────────────────────── */
/* 注意：这是 CEF 的 ABI 开头，所有接口结构体都以它打头。
 * sizeof = 0x28 (x64)：size(8) + add_ref(8) + release(8) + has_one_ref(8) + has_at_least_one_ref(8) */
typedef struct ncm_cef_base_ref_counted {
    size_t size;
    void (*add_ref)(struct ncm_cef_base_ref_counted *self);
    int  (*release)(struct ncm_cef_base_ref_counted *self);
    int  (*has_one_ref)(struct ncm_cef_base_ref_counted *self);
    int  (*has_at_least_one_ref)(struct ncm_cef_base_ref_counted *self);
} ncm_cef_base_ref_counted;

#define NCM_CEF_BASE_SIZE   0x28u   /* sizeof(cef_base_ref_counted_t) on x64 */

/* ── cef_client_t ─────────────────────────────────────────────────────────── */
/* 成员顺序（CEF 91）：base 之后 13 个 handler getter + on_process_message_received
 *
 *   0  0x28  get_context_menu_handler
 *   1  0x30  get_dialog_handler
 *   2  0x38  get_display_handler
 *   3  0x40  get_download_handler
 *   4  0x48  get_drag_handler
 *   5  0x50  get_find_handler
 *   6  0x58  get_focus_handler
 *   7  0x60  get_jsdialog_handler
 *   8  0x68  get_keyboard_handler
 *   9  0x70  get_life_span_handler
 *  10  0x78  get_load_handler          ← 我们要换掉的就是它
 *  11  0x80  get_render_handler
 *  12  0x88  get_request_handler
 *  13  0x90  on_process_message_received
 *   sizeof = 0x98
 *
 * 这个顺序不是猜的：文档里三个独立数字（0x38 / 0x68 / 0x78）只有
 * 在「base=0x28 + 上面这个顺序」下才同时成立（详见 docs/07 §7.2.2）。
 * 而且运行时会再用「扫描 + 校验」复核一遍。 */
#define NCM_CLIENT_IDX_LOAD_HANDLER   10u
#define NCM_CLIENT_SIZE_EXPECTED      0x98u

/* ── cef_load_handler_t ───────────────────────────────────────────────────── */
/* 这个结构体是**我们自己实现**的，所以布局由我们决定 —— 但 CEF 会按
 * 它自己的偏移来调用，因此我们必须和 CEF 对齐：
 *
 *   0  0x28  on_loading_state_change(browser, isLoading, canGoBack, canGoForward)
 *   1  0x30  on_load_start(browser, frame, httpStatusCode)
 *   2  0x38  on_load_end(browser, frame, httpStatusCode)
 *   3  0x40  on_load_error(browser, frame, errorCode, errorText, failedUrl)
 *   sizeof = 0x48
 *
 * 运行时用「槽位探针」实测确认（docs/07 §7.2.3）：每个槽位一个不同的 stub，
 * 谁被调用就打谁的编号 —— 这样 on_load_start / on_load_end 是**量出来的**。 */
typedef void (*ncm_on_loading_state_change)(void *self, void *browser, int is_loading,
                                            int can_go_back, int can_go_forward);
typedef void (*ncm_on_load_start)(void *self, void *browser, void *frame, int http_status);
typedef void (*ncm_on_load_end)(void *self, void *browser, void *frame, int http_status);
typedef void (*ncm_on_load_error)(void *self, void *browser, void *frame, int error_code,
                                  const cef_string_t *error_text, const cef_string_t *failed_url);

typedef struct ncm_cef_load_handler {
    ncm_cef_base_ref_counted base;
    void *slot[4];              /* 用 void* 存，便于探针模式整排替换 */
} ncm_cef_load_handler;

#define NCM_LOAD_HANDLER_SLOTS        4u
#define NCM_LOAD_IDX_STATE_CHANGE     0u
#define NCM_LOAD_IDX_START            1u
#define NCM_LOAD_IDX_END              2u
#define NCM_LOAD_IDX_ERROR            3u
#define NCM_LOAD_HANDLER_SIZE         0x48u

/* 探针模式可以多铺几个槽位，看看 CEF 到底碰哪儿 */
#define NCM_LOAD_HANDLER_PROBE_SLOTS  8u

/* ── cef_frame_t ──────────────────────────────────────────────────────────── */
/* frame 是由 libcef 实现的，成员顺序：
 *   ... load_request(10) load_url(11) execute_java_script(12) get_url(13) ...
 *   base=0x28 → execute_java_script = 0x28+12*8 = 0x88, get_url = 0x90
 *
 * 这两个数字**不硬编码使用**：运行时先扫描出数组基址，再用
 * get_url 的返回值做行为校验（必须长得像 URL），通过后才用；
 * 且整套调用都在 SEH 保护下，猜错不会崩进程。 */
typedef void (*ncm_frame_execute_js)(void *self, const cef_string_t *code,
                                     const cef_string_t *script_url, int start_line);
typedef cef_string_userfree_t (*ncm_frame_get_url)(void *self);

#define NCM_FRAME_IDX_LOAD_URL         11u
#define NCM_FRAME_IDX_EXECUTE_JS       12u
#define NCM_FRAME_IDX_GET_URL          13u

/* ── cef_app_t ────────────────────────────────────────────────────────────── */
/* 成员顺序：base 之后 on_before_command_line_processing / on_register_custom_schemes
 * / get_resource_bundle_handler / get_browser_process_handler / get_render_process_handler。
 *
 * ★ 关键性质：get_render_process_handler 是**最后一个成员**。
 *   所以它的偏移 = base.size - 8 —— 只需要在运行时读一次 size，
 *   完全不需要知道成员个数。这是为什么注入点选它，而不是 client_t
 *   （client_t 的 get_load_handler 在中间，必须知道成员数才能定位；
 *     第一次实机跑就是在这里翻车的，见 docs/07 §7.2.2）。
 *
 *   实机实测：base.size = 0x58 → 6 个成员，比我们记忆中的 5 个多一个。
 *   但偏移照样落在 app+0x50 且是代码指针（cloudmusic.dll+0x50A210），
 *   get_render_process_handler 被替换后 CEF 立刻开始向我们要 handler。
 *   **成员数记错不影响正确性** —— 这正是选"最后一个成员"的价值。 */
#define NCM_APP_EXPECTED_SIZE        0x58u   /* 实测值；代码只用 size-8，不依赖它 */

/* ── cef_execute_process ──────────────────────────────────────────────────── */
/* int cef_execute_process(const cef_main_args_t* args, cef_app_t* app, void* sandbox_info)
 * ★ 子进程（renderer/gpu/utility）走的是这个函数，而
 *   app->get_render_process_handler() 是**在子进程里**被调的 —— 所以必须在
 *   cef_execute_process 里换掉它。（这一点是第一次实机跑之后才对齐的：
 *   cef_initialize 只在浏览器进程里调，光hook它进不了页面。） */
typedef int (*ncm_execute_process_fn)(const void *args, void *app, void *sandbox_info);

/* ── cef_render_process_handler_t ─────────────────────────────────────────── */
/* 这个结构体由**我们**实现。CEF 会按它自己的偏移调用，所以布局必须一致。
 *
 * 成员顺序（CEF 91，10 个）：
 *   0  0x28  on_render_thread_created(extra_info)
 *   1  0x30  on_web_kit_initialized()
 *   2  0x38  on_browser_created(browser, extra_info)
 *   3  0x40  on_browser_destroyed(browser)
 *   4  0x48  get_load_handler()          → 返回指针
 *   5  0x50  on_context_created(browser, frame, context)   ← 注入点
 *   6  0x58  on_context_released(browser, frame, context)
 *   7  0x60  on_uncaught_exception(browser, frame, context, exception, trace)
 *   8  0x68  on_focused_node_changed(browser, frame, node)
 *   9  0x70  on_process_message_received(browser, frame, source, message)
 *   sizeof = 0x78
 *
 * 但**不依赖**这张表下结论：20 个槽位全部铺成"转发到原实现 + 记日志"的通用桩，
 * 谁的第三个参数能过 frame 校验，谁就是 on_context_created。
 * 这样即使顺序记错了也不会错注射、不会崩 —— 会自己找到正确的那一个。 */
typedef void *(*ncm_rph_slot_fn)(void *self, void *a, void *b, void *c, void *d, void *e);

#define NCM_RPH_SLOTS                 20u
#define NCM_RPH_EXPECTED_SIZE         0x78u
#define NCM_RPH_IDX_CONTEXT_CREATED   5u
#define NCM_RPH_IDX_GET_LOAD_HANDLER  4u

typedef struct ncm_cef_rph {
    ncm_cef_base_ref_counted base;
    ncm_rph_slot_fn slot[NCM_RPH_SLOTS];
} ncm_cef_rph;

/* ── cef_initialize ───────────────────────────────────────────────────────── */
typedef int (*ncm_cef_initialize_fn)(void *args, const void *settings, void *app, void *sandbox_info);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_CEF_CAPI_H */
