/**
 * ihook.c — 极简 x64 inline hook（绝对跳转 + trampoline）
 *
 * 为什么需要它：IAT 补丁只能拦「宿主从导入表调用的 API」。而路线 1 要拦的是
 * cloudmusic.dll **内部函数**（资源加载链），导出表里没有，只能改代码。
 *
 * 做法（不依赖任何反汇编库）：
 *   - 目标函数序言的前 N 字节，由调用方保证是「整数条完整指令」且不含
 *     RIP 相对寻址（用 IDA 看过，这里选了 3 条 mov = 15 字节）。
 *   - 把这 N 字节原样抄到 trampoline，末尾接一条「绝对跳回 原函数+N」。
 *   - 目标处写入 `FF 25 00000000 <qword hook>` + nop 填充。
 *
 * 约束：只能用于「调用方确认过序言」的函数；装之前会校验原始字节。
 */
#include "ihook.h"
#include "util.h"
#include <windows.h>
#include <string.h>

struct ncm_ihook {
    unsigned char *target;
    unsigned char *tramp;
    int            len;
    unsigned char  expect[24];
    int            installed;
};

int ncm_ihook_install(ncm_ihook **out, void *target, const unsigned char *expect,
                      int patch_len, void *hook)
{
    if (!out || !target || !expect || patch_len < 14 || patch_len > 24) return 0;
    *out = NULL;

    unsigned char *t = (unsigned char *)target;

    /* 版本校验：字节不符就不碰（换版本/换构建时必须先重新确认） */
    if (memcmp(t, expect, (size_t)patch_len) != 0) {
        char got[160] = ""; char want[160] = "";
        for (int i = 0; i < patch_len; i++) {
            char tmp[8];
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02X ", t[i]);
            strcat_s(got, sizeof(got), tmp);
            _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02X ", expect[i]);
            strcat_s(want, sizeof(want), tmp);
        }
        ncm_trace("ihook: %p 序言不符，拒绝打补丁", target);
        ncm_trace("ihook:   实际 = %s", got);
        ncm_trace("ihook:   预期 = %s", want);
        return 0;
    }

    unsigned char *tramp = (unsigned char *)VirtualAlloc(NULL, 64,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { ncm_trace("ihook: trampoline 分配失败 err=%lu", GetLastError()); return 0; }

    /* trampoline = 原序言字节 + 绝对跳回 target+patch_len */
    memcpy(tramp, t, (size_t)patch_len);
    int p = patch_len;
    tramp[p++] = 0xFF; tramp[p++] = 0x25;
    tramp[p++] = 0x00; tramp[p++] = 0x00; tramp[p++] = 0x00; tramp[p++] = 0x00;
    *(uintptr_t *)(tramp + p) = (uintptr_t)(t + patch_len);
    p += 8;

    /* 目标处：jmp [rip+0] → hook */
    unsigned char body[32];
    int q = 0;
    body[q++] = 0xFF; body[q++] = 0x25;
    body[q++] = 0x00; body[q++] = 0x00; body[q++] = 0x00; body[q++] = 0x00;
    *(uintptr_t *)(body + q) = (uintptr_t)hook;
    q += 8;
    while (q < patch_len) body[q++] = 0x90;

    DWORD old = 0;
    if (!VirtualProtect(t, (SIZE_T)patch_len, PAGE_EXECUTE_READWRITE, &old)) {
        ncm_trace("ihook: VirtualProtect 失败 err=%lu", GetLastError());
        VirtualFree(tramp, 0, MEM_RELEASE);
        return 0;
    }
    memcpy(t, body, (size_t)patch_len);
    VirtualProtect(t, (SIZE_T)patch_len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, (SIZE_T)patch_len);

    ncm_ihook *h = (ncm_ihook *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*h));
    if (!h) { VirtualFree(tramp, 0, MEM_RELEASE); return 0; }
    h->target = t;
    h->tramp = tramp;
    h->len = patch_len;
    memcpy(h->expect, expect, (size_t)patch_len);
    h->installed = 1;
    *out = h;

    ncm_trace("ihook: ✅ 已 hook %p（%d 字节，trampoline=%p，hook=%p）",
              target, patch_len, tramp, hook);
    return 1;
}

void *ncm_ihook_tramp(ncm_ihook *h)
{
    return h ? (void *)h->tramp : NULL;
}
