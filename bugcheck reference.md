# BugCheck Quick-Reference — camera/USB driver debugging

When the Xbox bugchecks, the fatal handler (CerBios LCD / debug screen) shows a
**BugCheck code** (Arg1) plus up to 4 arguments. This is the lookup for the codes
you'll actually hit doing USB/iso driver work, with what each *usually* means in
*our* context. Full table: `bugcodes.h` (233 codes). Companion: BUILD_SPEC §0
(instrumentation) — pair the code with the EIP→.map resolver to land on the function.

> Reading the screen: Arg1 = bugcheck code (below). For exception bugchecks
> (0x1E, 0x8E), the args carry the **exception code, faulting EIP, and address** —
> resolve the EIP against your linker `.MAP` to find which of your functions died.

## The ones you'll actually see (ranked by likelihood in this project)

### Pointer / memory bugs — the #1 category for a new driver
| Code | Name | What it almost always means for us |
|---|---|---|
| `0x0000000A` | IRQL_NOT_LESS_OR_EQUAL | Bad pointer dereference (NULL/garbage) at raised IRQL. The classic "I passed a bad pointer to SubmitRequest" or used a freed device-extension. Arg1=address, Arg4=EIP. |
| `0x000000D1` | DRIVER_IRQL_NOT_LESS_OR_EQUAL | Same as 0xA but explicitly a driver touching pageable/bad memory at DISPATCH. Our completion routines run at raised IRQL — a bad deref there lands here. |
| `0x00000050` | PAGE_FAULT_IN_NONPAGED_AREA | Touched an unmapped/freed address. Often: used an iso buffer after free, or a stale `EndpointHandle`. |
| `0x0000001E` | KMODE_EXCEPTION_NOT_HANDLED | Unhandled CPU exception (access violation etc.). Generic crash; Arg2=exception code, Arg3=EIP → resolve it. Very common early. THE one you'll see most. |
| `0x000000BE` | ATTEMPTED_WRITE_TO_READONLY_MEMORY | Wrote through a const/descriptor pointer (e.g. writing into a Get*Descriptor() result, which is `const`). |

### IRQL / synchronization bugs — show up once you're streaming
| Code | Name | Meaning for us |
|---|---|---|
| `0x00000008` | IRQL_NOT_DISPATCH_LEVEL | Called something requiring DISPATCH at the wrong level. |
| `0x00000009` | IRQL_NOT_GREATER_OR_EQUAL | Inverse — called a raised-IRQL-only API too low. |
| `0x000000C8` | IRQL_UNEXPECTED_VALUE | IRQL got corrupted — usually a mismatched raise/lower in a completion path. |
| `0x0000000F` | SPIN_LOCK_ALREADY_OWNED | Re-acquired a lock you hold (double-lock in the frame path). |
| `0x00000010` | SPIN_LOCK_NOT_OWNED | Released a lock you don't hold. |
| `0x0000000C` | MAXIMUM_WAIT_OBJECTS_EXCEEDED | Too many KEVENTs in a wait — watch the sync-completion pattern. |
| `0x000000E3` | RESOURCE_NOT_OWNED | Released a resource not owned. |

### Pool / allocation bugs — from buffer management
| Code | Name | Meaning for us |
|---|---|---|
| `0x00000019` | BAD_POOL_HEADER | Corrupted an allocation header — wrote past an iso buffer, or double-freed. |
| `0x000000C2` | BAD_POOL_CALLER | Bad pool call — wrong free routine, or freeing contiguous mem with the wrong API (use `MmFreeContiguousMemory` for `MmAllocateContiguousMemory`!). |
| `0x000000C5` | DRIVER_CORRUPTED_EXPOOL | Pool corruption traced to a driver — buffer overrun in frame assembly. |
| `0x000000C6` | DRIVER_CAUGHT_MODIFYING_FREED_POOL | Used memory after free — classic iso double-buffer race. |
| `0x0000004E` | PFN_LIST_CORRUPT | Physical page list corrupt — misuse of contiguous/physical memory. |

### Init / hang — Phase 1 and streaming stalls
| Code | Name | Meaning for us |
|---|---|---|
| `0x00000031` | PHASE0_INITIALIZATION_FAILED | Very early init failure (before our code, usually). |
| `0x00000032` | PHASE1_INITIALIZATION_FAILED | System init failed — if XInitDevices path explodes during bring-up. |
| `0x0000009F` | DRIVER_POWER_STATE_FAILURE | Power-transition mishandled (Save/RestoreState). |
| `0x00000012` | TRAP_CAUSE_UNKNOWN | Unclassified trap — least helpful; lean on the EIP. |
| `0x0000002E` | DATA_BUS_ERROR | Hardware-level bad access — can indicate DMA to a bad physical address (contiguous buffer issue). |

*(Note: there is no dedicated "thread stuck in driver" code in the Xbox `bugcodes.h`.
A hang with no bugcheck at all — frozen box, no LCD code — is the signature of an iso
completion routine that blocks or spins. Treat "froze, no code" as "a completion
didn't return fast.")*

## Debugging workflow (pairs with this table)
1. Read Arg1 → look up the code here → it tells you the **bug CLASS**.
2. Read the EIP arg (Arg4 for 0xA/0xD1, Arg2/3 for 0x1E/0x8E) → resolve against the
   linker `.MAP` → the **function** that died.
3. Class + function usually = root cause. (e.g. `0xD1` in your completion routine =
   bad pointer at raised IRQL in frame handling → check the device-extension/handle.)
4. If it's a pool/memory code (`0x19/C2/C5/C6/4E`), suspect the iso buffer lifecycle:
   wrong alloc/free pairing, overrun, or use-after-free in double-buffering.
5. **Hang with NO code** (frozen box, blank/last LCD) = a completion routine that
   blocks or spins — completions must be fast and non-blocking. There's no dedicated
   bugcheck for this on Xbox; the *absence* of a code with a frozen box is the tell.

## Project-specific watch list (the bugs THIS driver is prone to)
- **Wrong free for contiguous memory** → `0xC2`. Always pair
  `MmAllocateContiguousMemory` with `MmFreeContiguousMemory`.
- **Writing into a `const` descriptor** (Get*Descriptor returns `const*`) → `0xBE`.
- **Stale `EndpointHandle`** after close, reused in a URB → `0x50`/`0xA`.
- **Bad pointer in a completion routine** (raised IRQL) → `0xD1`.
- **Blocking/spinning in the iso completion** → frozen box, no bugcheck code.
- **Double-buffer use-after-free** under streaming → `0xC6`.
- **Bad `_PNP_CLASS_ID` / device-extension not set** before use → `0xA`.