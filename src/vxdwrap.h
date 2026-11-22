/*
 *     Copyright (C) 2026  Mustafa Ali
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * ==========================================================================
 *
 * vxdwrap.h -- C wrappers for IOS and IFSMgr VxD services
 *
 * Provides inline and NAKED function wrappers that let C code
 * call IOS and IFSMgr VxD services without inline assembly in
 * every call site.
 *
 * The Windows 95/98 DDK headers are heavily assembly-oriented. IOS.H
 * contains only an ASM comment block for the service table, and
 * there is no IFSMGR.H at all. This header fills that gap,
 * modeled on Walter Oney's IOSDCLS.H and IFSMGR.H from the LPFS
 * example in Systems Programming for Windows 95.
 *
 * Functionality in this file:
 *   1. IOS VxD service table declaration
 *   2. IOS VxD service wrappers (VxDCall)
 *   3. ILB VxD service wrappers (NAKED + jmp)
 *   4. ISP convenience wrappers
 *   5. IFSMgr VxD service table declaration
 *   6. IFSMgr VxD service wrappers (NAKED + VxDJmp)
 *   7. IFS.H helper macros
 */

#ifndef VXDWRAP_H
#define VXDWRAP_H

#ifndef FIELDOFFSET
#define FIELDOFFSET(type, field) ((DWORD)(&((type *)0)->field))
#endif /* FIELDOFFSET */

/* ========================================================================== */
/* IOS.H is useless for C-language users, so we declare the service table     */
/* here so that VxDCall / VxDJmp macros can resolve the service ordinals.     */
/* ========================================================================== */

#ifndef IOS_DEVICE_ID
#define IOS_DEVICE_ID   0x0010
#endif /* IOS_DEVICE_ID */

#define IOS_Service Declare_Service
#pragma warning(disable:4003)   /* not enough parameters */

Begin_Service_Table(IOS)

IOS_Service(IOS_Get_Version, LOCAL)
IOS_Service(IOS_BD_Register_Device)
IOS_Service(IOS_Find_Int13_Drive)
IOS_Service(IOS_Get_Device_List)
IOS_Service(IOS_SendCommand)
IOS_Service(IOS_BD_Command_Complete)
IOS_Service(IOS_Synchronous_Command)
IOS_Service(IOS_Register)
IOS_Service(IOS_Requestor_Service)
IOS_Service(IOS_Exclusive_Access)
IOS_Service(IOS_Send_Next_Command)
IOS_Service(IOS_Set_Async_Time_Out)
IOS_Service(IOS_Signal_Semaphore_No_Switch)
IOS_Service(IOSIdleStatus)
IOS_Service(IOSMapIORSToI24)
IOS_Service(IOSMapIORSToI21)

End_Service_Table(IOS)

#pragma warning(default:4003)

/* ========================================================================== */
/* C-callable inline wrappers for the IOS services we need. Each uses         */
/* VxDCall with register-based calling conventions,					          */
/* ========================================================================== */

#pragma warning(disable:4035)   /* missing return value */

#undef NAKED
#define NAKED __declspec(naked)

/*
 * IOS_Get_Version -- Check IOS version.
 * Returns IOS version in EAX, or 0 if IOS not present (carry set).
 */
VXDINLINE DWORD IOS_Get_Version(void) {
    VxDCall(IOS_Get_Version)
    _asm jnc ok
    _asm xor eax, eax
    _asm ok:
}

/*
 * IOS_Register -- Register a driver with IOS via a DRP.
 * drp: pointer to the Driver Registration Packet
 *
 * IOS_Register takes its argument on the stack (push drp, then VxDCall).
 */
VXDINLINE void IOS_Register(PDRP drp) {
    _asm push drp
    VxDCall(IOS_Register)
}

/*
 * IOS_SendCommand -- Send an I/O request to a device.
 * ior: pointer to the I/O Request packet (passed in ESI)
 * dcb: pointer to the Device Control Block (passed in EDI)
 */
VXDINLINE void IOS_SendCommand(PIOR ior, PDCB dcb) {
    _asm mov esi, ior
    _asm mov edi, dcb
    VxDCall(IOS_SendCommand)
}

/*
 * IOSMapIORSToI21 -- Map an IOR status code to an INT 21h error code.
 * iors: IOR status code (passed in ECX)
 * Returns: INT 21h error code in EAX
 */
VXDINLINE DWORD IOSMapIORSToI21(DWORD iors) {
    _asm mov ecx, iors
    VxDCall(IOSMapIORSToI21)
}

/*
 * IOSMapIORSToI24 -- Map an IOR status/flags to an INT 24h error code.
 * iors: IOR status code (passed in EAX)
 * iorf: IOR flags (passed in ECX)
 * Returns: INT 24h error code in EAX
 */
VXDINLINE DWORD IOSMapIORSToI24(DWORD iors, DWORD iorf) {
    _asm mov eax, iors
    _asm mov ecx, iorf
    VxDCall(IOSMapIORSToI24)
}

/* ========================================================================== */
/*                                                                            */
/* The ILB (IOS Linkage Block) is filled in by IOS during IOS_Register().     */
/* Its fields are function pointers. ILB_service_rtn and                      */
/* ILB_io_criteria_rtn take their argument on the stack, so we use            */
/* NAKED + jmp tail-call: the compiler pushes the arg, the naked wrapper      */
/* jumps to the function pointer, the service sees the arg on the stack.      */
/*                                                                            */
/* THESE DEFINITIONS ASSUME YOU HAVE AN ILB NAMED "theILB" (defined in        */
/* devdcl.asm).                                                               */
/* ========================================================================== */

extern ILB theILB;

/*
 * IlbService -- IOS service dispatcher (stack-based, single arg).
 * isp: pointer to an ISP packet
 * Returns: result code in EAX
 */
VXDINLINE DWORD NAKED IlbService(PVOID isp) {
    _asm jmp [theILB.ILB_service_rtn]
}

/*
 * IlbIoCriteria -- I/O criteria check (stack-based, single arg).
 * ior: pointer to an IOR packet
 * Returns: criteria flags in EAX
 */
VXDINLINE DWORD NAKED IlbIoCriteria(PIOR ior) {
    _asm jmp [theILB.ILB_io_criteria_rtn]
}

/* ========================================================================== */
/* The DDK provides ISP packet structures (ISP.H) but no functions to fill    */
/* them in and dispatch them. These wrappers construct the packet on the      */
/* stack and call IlbService().                                               */
/* ========================================================================== */

/*
 * IspCreateIor -- Allocate an IOR (I/O Request) packet.
 * size:  size of the IOP to allocate
 * delta: offset from IOP base to IOR within the IOP
 * flags: allocation flags
 * Returns: pointer to the IOR, or NULL on failure
 */
VXDINLINE PIOR IspCreateIor(USHORT size, ULONG delta, UCHAR flags) {
    ISP_IOP_alloc isp = {{ISP_CREATE_IOP, 0}, size, delta, 0, flags};
    IlbService(&isp);
    if (isp.ISP_i_c_hdr.ISP_result == 0)
        return (PIOR)(isp.ISP_IOP_ptr + delta);
    return NULL;
}

/*
 * IspDeallocMem -- Deallocate memory previously allocated by IOS.
 * p: pointer to the memory block to free
 * Returns: TRUE on success, FALSE on failure
 */
VXDINLINE BOOL IspDeallocMem(PVOID p) {
    ISP_mem_dealloc isp = {{ISP_DEALLOC_MEM, 0}, (ULONG)p};
    IlbService(&isp);
    return isp.ISP_mem_da_hdr.ISP_result == 0;
}

/*
 * IspAllocMem -- Allocate memory from the IOS heap.
 * size:  number of bytes to allocate
 * type:  memory type flags
 * flags: allocation flags
 * Returns: pointer to allocated memory, or NULL on failure
 */
VXDINLINE PVOID IspAllocMem(USHORT size, USHORT type, UCHAR flags) {
    ISP_mem_alloc isp = {{ISP_ALLOC_MEM, 0}, size, 0, type, flags};
    IlbService(&isp);
    if (isp.ISP_mem_a_hdr.ISP_result == 0)
        return (PVOID)isp.ISP_mem_ptr;
    return NULL;
}

/*
 * IspGetDcb -- Get the DCB for a drive letter.
 * drive: drive number (0=A, 1=B, 2=C, etc.)
 * Returns: pointer to the DCB, or NULL if not found
 */
VXDINLINE PDCB IspGetDcb(ULONG drive) {
    ISP_dcb_get isp = {{ISP_GET_DCB, 0}, 0, drive};
    IlbService(&isp);
    if (isp.ISP_g_d_hdr.ISP_result == 0)
        return (PDCB)isp.ISP_g_d_dcb;
    return NULL;
}

/* ========================================================================== */
/*                                                                            */
/* The DDK has NO C header for IFSMgr -- only IFSMGR.INC (assembly).          */
/* We declare the full service table here so VxDCall/VxDJmp macros resolve.   */
/* Service order must match IFSMGR.INC exactly.                               */
/* ========================================================================== */

#ifndef IFSMgr_DEVICE_ID
#define IFSMgr_DEVICE_ID    0x0040
#endif /* IFSMgr_DEVICE_ID */

#undef IFSMgr_Service
#define IFSMgr_Service Declare_Service
#pragma warning(disable:4003)   /* not enough parameters */

Begin_Service_Table(IFSMgr)

IFSMgr_Service(IFSMgr_Get_Version)
IFSMgr_Service(IFSMgr_RegisterMount)
IFSMgr_Service(IFSMgr_RegisterNet)
IFSMgr_Service(IFSMgr_RegisterMailSlot)
IFSMgr_Service(IFSMgr_Attach)
IFSMgr_Service(IFSMgr_Detach)
IFSMgr_Service(IFSMgr_Get_NetTime)
IFSMgr_Service(IFSMgr_Get_DOSTime)
IFSMgr_Service(IFSMgr_SetupConnection)
IFSMgr_Service(IFSMgr_DerefConnection)
IFSMgr_Service(IFSMgr_ServerDOSCall)
IFSMgr_Service(IFSMgr_CompleteAsync)
IFSMgr_Service(IFSMgr_RegisterHeap)
IFSMgr_Service(IFSMgr_GetHeap)
IFSMgr_Service(IFSMgr_RetHeap)
IFSMgr_Service(IFSMgr_CheckHeap)
IFSMgr_Service(IFSMgr_CheckHeapItem)
IFSMgr_Service(IFSMgr_FillHeapSpare)
IFSMgr_Service(IFSMgr_Block)
IFSMgr_Service(IFSMgr_Wakeup)
IFSMgr_Service(IFSMgr_Yield)
IFSMgr_Service(IFSMgr_SchedEvent)
IFSMgr_Service(IFSMgr_QueueEvent)
IFSMgr_Service(IFSMgr_KillEvent)
IFSMgr_Service(IFSMgr_FreeIOReq)
IFSMgr_Service(IFSMgr_MakeMailSlot)
IFSMgr_Service(IFSMgr_DeleteMailSlot)
IFSMgr_Service(IFSMgr_WriteMailSlot)
IFSMgr_Service(IFSMgr_PopUp)
IFSMgr_Service(IFSMgr_printf)
IFSMgr_Service(IFSMgr_AssertFailed)
IFSMgr_Service(IFSMgr_LogEntry)
IFSMgr_Service(IFSMgr_DebugMenu)
IFSMgr_Service(IFSMgr_DebugVars)
IFSMgr_Service(IFSMgr_GetDebugString)
IFSMgr_Service(IFSMgr_GetDebugHexNum)
IFSMgr_Service(IFSMgr_NetFunction)
IFSMgr_Service(IFSMgr_DoDelAllUses)
IFSMgr_Service(IFSMgr_SetErrString)
IFSMgr_Service(IFSMgr_GetErrString)
IFSMgr_Service(IFSMgr_SetReqHook)
IFSMgr_Service(IFSMgr_SetPathHook)
IFSMgr_Service(IFSMgr_UseAdd)
IFSMgr_Service(IFSMgr_UseDel)
IFSMgr_Service(IFSMgr_InitUseAdd)
IFSMgr_Service(IFSMgr_ChangeDir)
IFSMgr_Service(IFSMgr_DelAllUses)
IFSMgr_Service(IFSMgr_CDROM_Attach)
IFSMgr_Service(IFSMgr_CDROM_Detach)
IFSMgr_Service(IFSMgr_Win32DupHandle)
IFSMgr_Service(IFSMgr_Ring0_FileIO)
IFSMgr_Service(IFSMgr_Win32_Get_Ring0_Handle)
IFSMgr_Service(IFSMgr_Get_Drive_Info)
IFSMgr_Service(IFSMgr_Ring0GetDriveInfo)
IFSMgr_Service(IFSMgr_BlockNoEvents)
IFSMgr_Service(IFSMgr_NetToDosTime)
IFSMgr_Service(IFSMgr_DosToNetTime)
IFSMgr_Service(IFSMgr_DosToWin32Time)
IFSMgr_Service(IFSMgr_Win32ToDosTime)
IFSMgr_Service(IFSMgr_NetToWin32Time)
IFSMgr_Service(IFSMgr_Win32ToNetTime)
IFSMgr_Service(IFSMgr_MetaMatch)
IFSMgr_Service(IFSMgr_TransMatch)
IFSMgr_Service(IFSMgr_CallProvider)
IFSMgr_Service(UniToBCS)
IFSMgr_Service(UniToBCSPath)
IFSMgr_Service(BCSToUni)
IFSMgr_Service(UniToUpper)
IFSMgr_Service(UniCharToOEM)
IFSMgr_Service(CreateBasis)
IFSMgr_Service(MatchBasisName)
IFSMgr_Service(AppendBasisTail)
IFSMgr_Service(FcbToShort)
IFSMgr_Service(ShortToFcb)
IFSMgr_Service(IFSMgr_ParsePath)
IFSMgr_Service(Query_PhysLock)
IFSMgr_Service(_VolFlush)
IFSMgr_Service(NotifyVolumeArrival)
IFSMgr_Service(NotifyVolumeRemoval)
IFSMgr_Service(QueryVolumeRemoval)
IFSMgr_Service(IFSMgr_FSDUnmountCFSD)
IFSMgr_Service(IFSMgr_GetConversionTablePtrs)
IFSMgr_Service(IFSMgr_CheckAccessConflict)
IFSMgr_Service(IFSMgr_LockFile)
IFSMgr_Service(IFSMgr_UnlockFile)
IFSMgr_Service(IFSMgr_RemoveLocks)
IFSMgr_Service(IFSMgr_CheckLocks)
IFSMgr_Service(IFSMgr_CountLocks)
IFSMgr_Service(IFSMgr_ReassignLockFileInst)
IFSMgr_Service(IFSMgr_UnassignLockList)
IFSMgr_Service(IFSMgr_MountChildVolume)
IFSMgr_Service(IFSMgr_UnmountChildVolume)
IFSMgr_Service(IFSMgr_SwapDrives)
IFSMgr_Service(IFSMgr_FSDMapFHtoIOREQ)
IFSMgr_Service(IFSMgr_FSDParsePath)
IFSMgr_Service(IFSMgr_FSDAttachSFT)
IFSMgr_Service(IFSMgr_GetTimeZoneBias)
IFSMgr_Service(IFSMgr_PNPEvent)
IFSMgr_Service(IFSMgr_RegisterCFSD)
IFSMgr_Service(IFSMgr_Win32MapExtendedHandleToSFT)
IFSMgr_Service(IFSMgr_DbgSetFileHandleLimit)
IFSMgr_Service(IFSMgr_Win32MapSFTToExtendedHandle)
IFSMgr_Service(IFSMgr_FSDGetCurrentDrive)
IFSMgr_Service(IFSMgr_InstallFileSystemApiHook)
IFSMgr_Service(IFSMgr_RemoveFileSystemApiHook)
IFSMgr_Service(IFSMgr_RunScheduledEvents)
IFSMgr_Service(IFSMgr_CheckDelResource)
IFSMgr_Service(IFSMgr_Win32GetVMCurdir)
IFSMgr_Service(IFSMgr_SetupFailedConnection)
IFSMgr_Service(_GetMappedErr)
IFSMgr_Service(ShortToLossyFcb)
IFSMgr_Service(IFSMgr_GetLockState)
IFSMgr_Service(BcsToBcs)
IFSMgr_Service(IFSMgr_SetLoopback)
IFSMgr_Service(IFSMgr_ClearLoopback)
IFSMgr_Service(IFSMgr_ParseOneElement)
IFSMgr_Service(BcsToBcsUpper)

End_Service_Table(IFSMgr)

#pragma warning(default:4003)

/* ========================================================================== */
/* Nearly every IFSMgr wrapper uses NAKED + VxDJmp.							  */
/*                                                                            */
/*   1. The compiler generates a normal C call site: push args, call wrapper. */
/*   2. The wrapper is naked -- no prologue, no push ebp, no sub esp.         */
/*      The stack on entry is: [return-to-caller] [arg1] [arg2] ...           */
/*   3. VxDJmp expands to int 20h + service ID. On first execution, the       */
/*      VMM patches this to a direct jmp to the service entry point.          */
/*   4. The jmp is direct: the service sees the caller's stack frame          */
/*      directly. When the service executes ret, it returns to the caller.    */
/*   5. After the first invocation, the int 20h is gone.                      */
/*                                                                            */
/* VxDCall (which patches to a call, not jmp) is only used for the few        */
/* services that need to inspect the result after the service returns --      */
/* e.g., checking the carry flag for IFSMgr_Get_Version.                      */
/* ========================================================================== */

/*
 * IFSMgr_Get_Version -- Check IFSMgr version.
 * Register-based, no args. Returns version in EAX, 0 if not present.
 */
DWORD VXDINLINE IFSMgr_Get_Version(void) {
    VxDCall(IFSMgr_Get_Version)
    _asm jnc okay
    _asm xor eax, eax
    _asm okay:
}

/*
 * IFSMgr_RegisterMount -- Register a local FSD mount callback.
 * pfn:       pointer to the mount/volume function
 * version:   IFSMgr version expected (IFSMGRVERSION)
 * defoption: NORMAL_FSD or DEFAULT_FSD
 * Returns:   FSD ID (>= 0) on success, negative on failure
 */
int VXDINLINE NAKED IFSMgr_RegisterMount(pIFSFunc pfn, UINT version,
                                          UINT defoption) {
    VxDJmp(IFSMgr_RegisterMount)
}

/*
 * IFSMgr_Block -- Block the current thread until woken.
 * key: arbitrary 32-bit key used to match Block/Wakeup pairs
 */
VOID VXDINLINE NAKED IFSMgr_Block(DWORD key) {
    VxDJmp(IFSMgr_Block)
}

/*
 * IFSMgr_Wakeup -- Wake a thread blocked with the given key.
 * key: the same key passed to IFSMgr_Block
 */
VOID VXDINLINE NAKED IFSMgr_Wakeup(DWORD key) {
    VxDJmp(IFSMgr_Wakeup)
}

/*
 * IFSMgr_CompleteAsync -- Signal async I/O completion on an ioreq.
 * pir: pointer to the ioreq being completed
 */
VOID VXDINLINE NAKED IFSMgr_CompleteAsync(pioreq pir) {
    VxDJmp(IFSMgr_CompleteAsync)
}

/*
 * IFSMgr_Get_DOSTime -- Get current time in DOS format.
 * Returns: dos_time structure with date and time fields
 */
dos_time VXDINLINE NAKED IFSMgr_Get_DOSTime(void) {
    VxDJmp(IFSMgr_Get_DOSTime)
}

/*
 * IFSMgr_DosToWin32Time -- Convert DOS time to Win32 FILETIME.
 * dt: DOS time value
 * Returns: _FILETIME structure
 */
_FILETIME VXDINLINE NAKED IFSMgr_DosToWin32Time(dos_time dt) {
    VxDJmp(IFSMgr_DosToWin32Time)
}

/*
 * IFSMgr_Win32ToDosTime -- Convert Win32 FILETIME to DOS time.
 * ft: Win32 FILETIME value
 * Returns: dos_time structure
 */
dos_time VXDINLINE NAKED IFSMgr_Win32ToDosTime(_FILETIME ft) {
    VxDJmp(IFSMgr_Win32ToDosTime)
}

/*
 * UniToBCS -- Convert Unicode string to BCS (byte character set).
 * pBCS:      output buffer for BCS string
 * pUni:      input Unicode string
 * length:    length of Unicode string in bytes
 * maxLength: maximum bytes to write to pBCS
 * charSet:   target character set (BCS_WANSI, BCS_OEM, etc.)
 * Returns:   number of bytes written
 */
UINT VXDINLINE NAKED UniToBCS(PBYTE pBCS, string_t pUni,
                               UINT length, UINT maxLength,
                               UINT charSet) {
    VxDJmp(UniToBCS)
}

/*
 * UniToBCSPath -- Convert Unicode path to BCS path.
 * pBCSPath:   output buffer for BCS path
 * pUniPath:   input Unicode path (PathElement)
 * maxLength:  maximum bytes to write
 * charSet:    target character set
 * Returns:    number of bytes written
 */
UINT VXDINLINE NAKED UniToBCSPath(PBYTE pBCSPath, PathElement* pUniPath,
                                   UINT maxLength, int charSet) {
    VxDJmp(UniToBCSPath)
}

/*
 * BCSToUni -- Convert BCS string to Unicode.
 * pUni:    output buffer for Unicode string
 * pBCS:    input BCS string
 * length:  length of BCS string in bytes
 * charSet: source character set
 * Returns: number of bytes written to pUni
 */
UINT VXDINLINE NAKED BCSToUni(string_t pUni, PBYTE pBCS,
                               UINT length, int charSet) {
    VxDJmp(BCSToUni)
}

/*
 * UniToUpper -- Convert Unicode string to uppercase.
 * pDst:   output buffer for uppercased string
 * pSrc:   input Unicode string
 * length: length in bytes
 * Returns: number of bytes written
 */
UINT VXDINLINE NAKED UniToUpper(string_t pDst, string_t pSrc,
                                 UINT length) {
    VxDJmp(UniToUpper)
}

/*
 * IFSMgr_MetaMatch -- Wildcard pattern matching on Unicode strings.
 * pPat:     Unicode pattern (may contain * and ? wildcards)
 * pName:    Unicode name to match against
 * matchSem: match semantics flags (e.g. UFLG_NT_DOS | UFLG_META)
 * Returns:  NON-ZERO if the name matches the pattern, 0 if it does not.
 *           (Confirmed against the vtoolsd regfsd sample.)
 */
UINT VXDINLINE NAKED IFSMgr_MetaMatch(string_t pPat, string_t pName,
                                        int matchSem) {
    VxDJmp(IFSMgr_MetaMatch)
}

/*
 * CreateBasis -- Create an 8.3 basis name from a long Unicode name.
 * pBasis: output buffer for the basis name
 * pLong:  input Unicode long name
 * length: length of the long name in bytes
 * Returns: number of characters in the basis name
 */
UINT VXDINLINE NAKED CreateBasis(string_t pBasis, string_t pLong,
                                  UINT length) {
    VxDJmp(CreateBasis)
}

/*
 * AppendBasisTail -- Append a numeric tail (~N) to a basis name.
 * pBasis: basis name buffer (modified in place)
 * tail:   numeric tail value
 * Returns: result code
 */
int VXDINLINE NAKED AppendBasisTail(string_t pBasis, int tail) {
    VxDJmp(AppendBasisTail)
}

/*
 * MatchBasisName -- Check if a name matches a basis name pattern.
 * pBasis: basis name to match against
 * pName:  name to check
 * Returns: 0 if match, non-zero otherwise
 */
int VXDINLINE NAKED MatchBasisName(string_t pBasis, string_t pName) {
    VxDJmp(MatchBasisName)
}

/*
 * FcbToShort -- Convert an FCB-format name to 8.3 short name.
 * p83name:  output buffer for 8.3 name
 * pFCBName: input FCB-format name
 * trailDot: whether to include trailing dot
 * Returns:  result code
 */
int VXDINLINE NAKED FcbToShort(string_t p83name, string_t pFCBName,
                                int trailDot) {
    VxDJmp(FcbToShort)
}

/*
 * ShortToLossyFcb -- Convert 8.3 short name to FCB format (lossy).
 * pFCBName: output buffer for FCB-format name
 * p83Name:  input 8.3 short name
 * length:   length of the short name
 * Returns:  result code
 */
int VXDINLINE NAKED ShortToLossyFcb(string_t pFCBName, string_t p83Name,
                                      UINT length) {
    VxDJmp(ShortToLossyFcb)
}

/*
 * NotifyVolumeArrival -- Notify the system that a new volume is available.
 * drive: drive number (0=A, 1=B, 2=C, etc.)
 */
VOID VXDINLINE NAKED NotifyVolumeArrival(UINT drive) {
    VxDJmp(NotifyVolumeArrival)
}

/*
 * NotifyVolumeRemoval -- Notify the system that a volume has been removed.
 * drive: drive number (0=A, 1=B, 2=C, etc.)
 */
VOID VXDINLINE NAKED NotifyVolumeRemoval(UINT drive) {
    VxDJmp(NotifyVolumeRemoval)
}

#pragma warning(default:4035)


#endif /* VXDWRAP_H */