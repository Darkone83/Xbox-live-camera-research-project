/*
 * xbox_kernel.h -- kernel-primitive declarations for Xbox USB driver code.
 * Team Resurgent / Darkone83
 *
 * The Xbox kernel exports (Ke*, Mm*, DbgPrint, KEVENT, IRQL levels, ASSERT) live
 * in the DDK kernel headers (ntddk.h / ntos.h) that the leak's drivers include
 * BEFORE <xtl.h>. RXDK is a slimmed kit and may not ship those headers, so this
 * shim:
 *   1. tries to pull in the real header if the toolchain has it, else
 *   2. declares exactly what our driver uses, matching the kernel ABI.
 *
 * The declarations match the original Xbox kernel (xboxkrnl) signatures, verified
 * against usage in the leak source (usbsamp/xid/ohcd). If RXDK DOES provide these
 * via its own headers, this shim's fallback block is guarded out by the macros
 * the real header defines, so there's no conflict.
 *
 * If you get "already defined" conflicts, RXDK supplies these itself -- in that
 * case just remove the include of this file (or set XBOX_KERNEL_USE_RXDK).
 */
#ifndef __XBOX_KERNEL_H__
#define __XBOX_KERNEL_H__

 /* ---- 1. prefer the real DDK/kernel header if present ----
  * Most RXDK setups expose the kernel via <xtl.h>'s chain or <xboxkrnl.h>.
  * Try common names; if none exist, the fallback below provides the symbols.
  * (Edit this to match your RXDK if it uses a different header.) */
#if defined(XBOX_KERNEL_USE_RXDK)
  /* user asserts RXDK provides everything -- include nothing extra here */
#elif __has_include(<xboxkrnl/xboxkrnl.h>)
#  include <xboxkrnl/xboxkrnl.h>
#  define XBOX_KERNEL_PROVIDED 1
#elif __has_include(<xboxkrnl.h>)
#  include <xboxkrnl.h>
#  define XBOX_KERNEL_PROVIDED 1
#elif __has_include(<ntddk.h>)
#  include <ntddk.h>
#  define XBOX_KERNEL_PROVIDED 1
#endif

  /* ---- 2. fallback declarations (only if the real header wasn't found) ---- */
#ifndef XBOX_KERNEL_PROVIDED

#ifdef __cplusplus
extern "C" {
#endif

    /* IRQL */
    typedef UCHAR KIRQL, * PKIRQL;
#ifndef PASSIVE_LEVEL
#define PASSIVE_LEVEL   0
#define APC_LEVEL       1
#define DISPATCH_LEVEL  2
#endif

    /* KEVENT -- opaque-but-sized dispatcher object. The Xbox DISPATCHER_HEADER +
     * list entry is 0x10 bytes; declare a correctly-sized object so it can be a
     * struct member (fixes "incomplete type KEVENT"). Layout per ntddk. */
#ifndef _KEVENT_DEFINED
#define _KEVENT_DEFINED
    typedef struct _DISPATCHER_HEADER {
        UCHAR  Type;
        UCHAR  Absolute;
        UCHAR  Size;
        UCHAR  Inserted;
        LONG   SignalState;
        struct { struct _LIST_ENTRY* Flink, * Blink; } WaitListHead;  /* LIST_ENTRY */
    } DISPATCHER_HEADER;
    typedef struct _KEVENT {
        DISPATCHER_HEADER Header;
    } KEVENT, * PKEVENT;
#endif

    typedef enum _EVENT_TYPE { NotificationEvent = 0, SynchronizationEvent = 1 } EVENT_TYPE;

    /* Ke* primitives (signatures verified against leak usage) */
    VOID  __stdcall KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State);
    KIRQL __stdcall KeGetCurrentIrql(VOID);
    LONG  __stdcall KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
    LONG  __stdcall KeWaitForSingleObject(PVOID Object, ULONG WaitReason, ULONG WaitMode,
        BOOLEAN Alertable, PVOID Timeout);

    /* Mm* contiguous-memory (DMA buffers). Pair Allocate with FreeContiguous. */
    PVOID __stdcall MmAllocateContiguousMemory(ULONG NumberOfBytes);
    VOID  __stdcall MmFreeContiguousMemory(PVOID BaseAddress);

    /* Rtl / pool / debug */
    VOID  __cdecl  DbgPrint(const char* Format, ...);
#ifndef RtlZeroMemory
    VOID  __stdcall RtlZeroMemory(PVOID Destination, ULONG Length);
#endif

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* ASSERT -- debug-only check; no-op on retail. */
#ifndef ASSERT
#ifdef _DEBUG
#define ASSERT(exp) ((void)((exp) || (DbgPrint("ASSERT failed: %s (%s:%d)\n", #exp, __FILE__, __LINE__), 0)))
#else
#define ASSERT(exp) ((void)0)
#endif
#endif

#endif /* !XBOX_KERNEL_PROVIDED */

#endif /* __XBOX_KERNEL_H__ */