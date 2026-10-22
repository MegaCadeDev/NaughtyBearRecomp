# Naughty Bear's Direct3D library

Naughty Bear links the Xbox 360 XDK Direct3D library statically. The renderer port
follows Unleashed Recompiled: the public D3D functions are replaced with host
implementations by address, so each one has to be found in this executable.

## Extent

All PM4 packet construction lives in `0x82F20000-0x82F50000`; that range is the D3D
library (Sonic Unleashed's is at `0x82BD9000-0x82C01000`). The game's own renderer
layer calls into it from relatively few places (at most 38 call sites per function),
so the number of hooks needed should be manageable.

## Identified

| Address | Function | Evidence |
|-|-|-|
| `0x82F26FA0` | `Direct3D_CreateDevice` | 6 arguments; clears `*ppDevice` (r8); allocates a 0x5F00-byte, 128-aligned device; returns `E_OUTOFMEMORY` (0x8007000E) on failure; stores BehaviorFlags at device+0x5E80; writes `*ppDevice`; reaches `VdInitializeEngines` through `0x82F42E70` -> `0x82F42D68`. Called from `0x82AEE758` and `0x82B91250`. |
| `0x82F42D68` | internal engine init | calls `VdInitializeEngines`, `VdSetGraphicsInterruptCallback`, `ExRegisterTitleTerminateNotification` |
| `0x82F3C298` | internal ring buffer init | calls `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, `VdSetSystemCommandBufferGpuIdentifierAddress` |
| `0x82F33D50` | internal swap | calls `VdSwap`, `VdGetSystemCommandBuffer`, `VdPersistDisplay` |

Candidates still to confirm: `0x82F343B0` (reaches the swap; likely `D3DDevice_Present`
or `D3DDevice_Swap`).

## Device layout

The device is **0x5F00 bytes**, 0x100 more than Unleashed Recompiled's `GuestDevice`
(0x5E00), so this XDK version's layout differs and each field Unleashed reads
(dirty flags, render state function table, sampler states, shader constants,
vertex declaration, viewport) has to be located again.

## Method

`tools/nbanalyse` has the tools used:

- `callgraph XEX begin end`: per function in a range, external/internal caller counts,
  imports used and PM4 type-3 packet headers built (`opcode/count`).
- `xrefs XEX addr...`: every branch to an address and the function it sits in.
- `funcinfo`, `disas`, `peek`, `imports`.

Anchors come from kernel calls (`Vd*`, `MmAllocatePhysicalMemoryEx`), PM4 opcodes
(e.g. `DRAW_INDX` 0x22, `DRAW_INDX_2` 0x36, `SET_CONSTANT` 0x2D, `EVENT_WRITE` 0x46)
and argument shapes matching the XDK API.
