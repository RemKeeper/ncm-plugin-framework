/*
 * pet_hook.h —— 桌宠窗口鼠标交互旁路（子类化 WndProc → SSE 推给页面）
 */
#ifndef NCM_PET_HOOK_H
#define NCM_PET_HOOK_H

/** 启动钩子线程；内部 1.5s 轮询查找 CloudMusicDesktopPetOsrView 并挂上。 */
void ncm_pet_hook_start(void);

#endif
