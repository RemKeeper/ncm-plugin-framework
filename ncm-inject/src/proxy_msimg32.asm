;
; proxy_msimg32.asm — MSIMG32 五个导出的裸跳板（x64, MASM）
;
; x64 下 MSVC 不支持 inline asm，必须用独立 .asm 才能做"零开销转发"。
; 每个过程只有一条 jmp qword ptr [槽位]，寄存器/栈完全不被触碰。
;
; 组装：  ml64 /nologo /c /Fo build\proxy.obj proxy_msimg32.asm

.DATA
EXTERN g_pfn_vSetDdrawflag  : QWORD
EXTERN g_pfn_AlphaBlend     : QWORD
EXTERN g_pfn_DllInitialize  : QWORD
EXTERN g_pfn_GradientFill   : QWORD
EXTERN g_pfn_TransparentBlt : QWORD

.CODE

AheadLib_vSetDdrawflag PROC
    jmp qword ptr [g_pfn_vSetDdrawflag]
AheadLib_vSetDdrawflag ENDP

AheadLib_AlphaBlend PROC
    jmp qword ptr [g_pfn_AlphaBlend]
AheadLib_AlphaBlend ENDP

AheadLib_DllInitialize PROC
    jmp qword ptr [g_pfn_DllInitialize]
AheadLib_DllInitialize ENDP

AheadLib_GradientFill PROC
    jmp qword ptr [g_pfn_GradientFill]
AheadLib_GradientFill ENDP

AheadLib_TransparentBlt PROC
    jmp qword ptr [g_pfn_TransparentBlt]
AheadLib_TransparentBlt ENDP

END
