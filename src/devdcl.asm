;==============================================================================
;    Copyright (C) 2026  Mustafa Ali
;
; This program is free software; you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation; either version 2 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License along
; with this program; if not, write to the Free Software Foundation, Inc.,
; 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
;
;
;   DEVDCL.ASM -- VxD device declaration, DRP/ILB, and control procedure
;
;   APFS IFS Driver for Windows 9x
;
;==============================================================================

        .386p

        include vmm.inc
        include debug.inc
        include drp.inc
        include ilb.inc
        include ios.inc

;==============================================================================
;   IDATA segment -- discardable init data (DRP)
;
;   DRP_aer is set to 0 here; the C init code sets it before calling
;   IOS_Register (same pattern as UMSS which defines DRP in C).
;==============================================================================

VxD_IDATA_SEG

        PUBLIC  _theDRP
_theDRP DRP     <'APFS    ', DRP_FSD, 0, \
                offset32 _theILB, 'AppleFileSys    ', 0, 0, 0>

VxD_IDATA_ENDS

;==============================================================================
;   Locked data segment -- non-discardable (ILB)
;==============================================================================

VXD_LOCKED_DATA_SEG

        PUBLIC  _theILB
_theILB ILB     <>

VXD_LOCKED_DATA_ENDS

;==============================================================================
;   Virtual Device Declaration
;==============================================================================

Declare_Virtual_Device APFS, 1, 0, APFS_Control, \
        Undefined_Device_ID, UNDEFINED_INIT_ORDER, , , _theDRP

;==============================================================================
;   Control Procedure (locked code)
;
;   Use bare C function names with sCall -- the Control_Dispatch macro
;   handles name decoration automatically. This matches the UMSS pattern.
;==============================================================================

VxD_LOCKED_CODE_SEG

BeginProc APFS_Control

        Control_Dispatch SYS_DYNAMIC_DEVICE_INIT, APFS_Dynamic_Init, sCall
        Control_Dispatch SYS_DYNAMIC_DEVICE_EXIT, APFS_Dynamic_Exit, sCall
        clc
        ret

EndProc APFS_Control

;==============================================================================
;   _atexit stub -- required by the C compiler
;==============================================================================

BeginProc _atexit, PUBLIC

        xor     eax, eax
        ret

EndProc _atexit

VxD_LOCKED_CODE_ENDS

;==============================================================================
;   ICODE segment -- discardable init code
;
;   _IosRegister: called from C as void IosRegister(DRP* drp);
;   Uses the UMSS pattern with ArgVar/EnterProc/LeaveProc.
;==============================================================================

VxD_ICODE_SEG

public _IosRegister

BeginProc _IosRegister, esp
        ArgVar DrvPkt, DWORD

        EnterProc
        VxDCall IOS_Register <DrvPkt>
        LeaveProc

        Return

EndProc _IosRegister

VxD_ICODE_ENDS

;==============================================================================
;   64-bit arithmetic runtime helpers (locked code)
;
;   The MSVC ring-0 compiler emits calls to these CRT helpers for
;   64-bit operations. VXD-land does not have the C runtime so we need them.
;   These functions are copied from the MSVC implementations.
;
;   Calling conventions:
;     __allmul, __aulldiv, __aullrem: arguments on stack, callee-pops 16 bytes
;     __allshl, __aullshr: shift count in CL, value in EDX:EAX, register-only
;==============================================================================

VxD_LOCKED_CODE_SEG

;----------------------------------------------------------------------
; __allmul -- 64-bit multiply (signed and unsigned)
;
; EDX:EAX = [esp+4]:[esp+8] * [esp+12]:[esp+16]
; Callee pops 16 bytes.
;----------------------------------------------------------------------

public __allmul
__allmul PROC NEAR

        mov     eax, [esp+8]        ; A_hi
        mov     ecx, [esp+16]       ; B_hi
        or      ecx, eax
        mov     ecx, [esp+12]       ; B_lo
        jnz     short mul_hard

        ; simple: both fit in 32 bits
        mov     eax, [esp+4]        ; A_lo
        mul     ecx
        ret     16

mul_hard:
        push    ebx
        ; stack shifted by 4: A_lo=[esp+8] A_hi=[esp+12] B_lo=[esp+16] B_hi=[esp+20]
        mov     eax, [esp+12]       ; A_hi
        mul     ecx                 ; A_hi * B_lo
        mov     ebx, eax
        mov     eax, [esp+8]        ; A_lo
        mul     dword ptr [esp+20]  ; A_lo * B_hi
        add     ebx, eax
        mov     eax, [esp+8]        ; A_lo
        mul     ecx                 ; A_lo * B_lo -> EDX:EAX
        add     edx, ebx
        pop     ebx
        ret     16

__allmul ENDP

;----------------------------------------------------------------------
; __aulldiv -- unsigned 64-bit divide
;
; EDX:EAX = [esp+4]:[esp+8] / [esp+12]:[esp+16]
; Callee pops 16 bytes.
;----------------------------------------------------------------------

public __aulldiv
__aulldiv PROC NEAR

        push    ebx
        push    esi
        ; A_lo=[esp+12] A_hi=[esp+16] B_lo=[esp+20] B_hi=[esp+24]

        mov     eax, [esp+24]       ; B_hi
        or      eax, eax
        jnz     short div_big

        ; divisor fits in 32 bits
        mov     ecx, [esp+20]       ; B_lo
        mov     eax, [esp+16]       ; A_hi
        xor     edx, edx
        div     ecx
        mov     ebx, eax            ; hi quotient
        mov     eax, [esp+12]       ; A_lo
        div     ecx
        mov     edx, ebx
        jmp     short div_done

div_big:
        mov     ecx, eax            ; B_hi
        mov     ebx, [esp+20]       ; B_lo
        mov     edx, [esp+16]       ; A_hi
        mov     eax, [esp+12]       ; A_lo

@@:     shr     ecx, 1
        rcr     ebx, 1
        shr     edx, 1
        rcr     eax, 1
        or      ecx, ecx
        jnz     @B

        div     ebx
        mov     esi, eax            ; approximate quotient

        mul     dword ptr [esp+24]  ; q * B_hi
        mov     ecx, eax
        mov     eax, esi
        mul     dword ptr [esp+20]  ; q * B_lo
        add     edx, ecx

        cmp     edx, [esp+16]
        ja      short div_adj
        jb      short div_ok
        cmp     eax, [esp+12]
        jbe     short div_ok

div_adj:
        dec     esi

div_ok:
        xor     edx, edx
        mov     eax, esi

div_done:
        pop     esi
        pop     ebx
        ret     16

__aulldiv ENDP

;----------------------------------------------------------------------
; __aullrem -- unsigned 64-bit remainder
;
; EDX:EAX = [esp+4]:[esp+8] % [esp+12]:[esp+16]
; Callee pops 16 bytes.
;----------------------------------------------------------------------

public __aullrem
__aullrem PROC NEAR

        push    ebx
        ; A_lo=[esp+8] A_hi=[esp+12] B_lo=[esp+16] B_hi=[esp+20]

        mov     eax, [esp+20]       ; B_hi
        or      eax, eax
        jnz     short rem_big

        ; divisor fits in 32 bits
        mov     ecx, [esp+16]       ; B_lo
        mov     eax, [esp+12]       ; A_hi
        xor     edx, edx
        div     ecx
        mov     eax, [esp+8]        ; A_lo
        div     ecx
        mov     eax, edx            ; remainder
        xor     edx, edx
        jmp     short rem_done

rem_big:
        mov     ecx, eax            ; B_hi
        mov     ebx, [esp+16]       ; B_lo
        mov     edx, [esp+12]       ; A_hi
        mov     eax, [esp+8]        ; A_lo

@@:     shr     ecx, 1
        rcr     ebx, 1
        shr     edx, 1
        rcr     eax, 1
        or      ecx, ecx
        jnz     @B

        div     ebx                 ; approximate quotient in eax

        ; remainder = dividend - quotient * divisor
        mov     ecx, eax
        mul     dword ptr [esp+20]  ; q * B_hi
        push    eax                 ; save lo of (q * B_hi)
        mov     eax, ecx
        mul     dword ptr [esp+16+4] ; q * B_lo (stack shifted by push)
        pop     ebx                 ; lo of (q * B_hi)
        add     edx, ebx            ; EDX:EAX = q * divisor

        sub     eax, [esp+8]        ; subtract dividend_lo
        sbb     edx, [esp+12]       ; subtract dividend_hi
        neg     edx                 ; negate to get remainder
        neg     eax
        sbb     edx, 0

        ; if remainder >= divisor, subtract divisor
        jnc     short rem_done
        add     eax, [esp+16]
        adc     edx, [esp+20]

rem_done:
        pop     ebx
        ret     16

__aullrem ENDP

;----------------------------------------------------------------------
; __aullshr -- unsigned 64-bit shift right
;
; EDX:EAX >>= CL  (register calling convention, no stack args)
;----------------------------------------------------------------------

public __aullshr
__aullshr PROC NEAR

        cmp     cl, 64
        jae     short shr_zero
        cmp     cl, 32
        jae     short shr_32plus

        shrd    eax, edx, cl
        shr     edx, cl
        ret

shr_32plus:
        mov     eax, edx
        xor     edx, edx
        and     cl, 31
        shr     eax, cl
        ret

shr_zero:
        xor     eax, eax
        xor     edx, edx
        ret

__aullshr ENDP

;----------------------------------------------------------------------
; __allshl -- 64-bit shift left
;
; EDX:EAX <<= CL  (register calling convention, no stack args)
;----------------------------------------------------------------------

public __allshl
__allshl PROC NEAR

        cmp     cl, 64
        jae     short shl_zero
        cmp     cl, 32
        jae     short shl_32plus

        shld    edx, eax, cl
        shl     eax, cl
        ret

shl_32plus:
        mov     edx, eax
        xor     eax, eax
        and     cl, 31
        shl     edx, cl
        ret

shl_zero:
        xor     eax, eax
        xor     edx, edx
        ret

__allshl ENDP

VxD_LOCKED_CODE_ENDS

        end
