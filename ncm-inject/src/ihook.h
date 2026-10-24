/**
 * ihook.h — 极简 x64 inline hook
 */
#ifndef NCM_IHOOK_H
#define NCM_IHOOK_H

#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct ncm_ihook ncm_ihook;

/**
 * 安装 hook。
 * @param out       输出句柄
 * @param target    目标函数地址
 * @param expect    目标序言的前 patch_len 字节（用于版本校验，不符则拒绝）
 * @param patch_len 覆盖长度（>=14，且必须是完整指令边界）
 * @param hook      我们的替换函数
 */
int  ncm_ihook_install(ncm_ihook **out, void *target, const unsigned char *expect,
                       int patch_len, void *hook);
/** 取 trampoline（用来调用原函数） */
void *ncm_ihook_tramp(ncm_ihook *h);

#if defined(__cplusplus)
}
#endif

#endif /* NCM_IHOOK_H */
