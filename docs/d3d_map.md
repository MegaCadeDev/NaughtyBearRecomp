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

| `0x82F23D28` | `D3DResource_Release` | atomic decrement of the refcount at +4; releases the parent of sub-resources; calls the destructor when it reaches 0 |
| `0x82F237C0` | resource destructor (Unleashed's `DestructResource`) | called by Release at refcount 0 |
| `0x82F3AFB8` | `BlockOnFence` (internal) | spins until `*(device+0x2A90)` (GPU-written completed fence) reaches the fence |

### From runtime traces

Arguments observed with `NB_TRACE_D3D` (see below) against the XDK signatures.
Confidence: **high** = arguments match exactly; **likely** = consistent but unverified.

| Address | Function | Confidence | Observed arguments |
|-|-|-|-|
| `0x82F38988` | `D3DDevice_DrawVertices` | high | (dev, PrimitiveType, StartVertex, VertexCount); flushes dirty state and constants first |
| `0x82F38D78` | `D3DDevice_DrawIndexedVertices` | high | (dev, PrimitiveType, BaseVertexIndex, StartIndex, IndexCount); same flush |
| `0x82F38388` | `D3DDevice_DrawVerticesUP` | high | (dev, 4, VertexCount 0x120, pVertexStreamZeroData, Stride 0x1C) |
| `0x82F388E8` | `D3DDevice_DrawIndexedVerticesUP` | high | (dev, PrimitiveType, MinVertexIndex, NumVertices 12, IndexCount 30, pIndexData, IndexDataFormat, pVertexStreamZeroData, Stride on the stack); copies both into the command buffer through `0x82F383D0` |
| `0x82F37138` | `D3DDevice_BeginTiling` | high | (dev, Flags, Count, pTileRects, pClearColor, ClearZ in f1, ClearStencil in r9). The game renders with predicated tiling; Sonic does not, so this is a new hook (a clear on PC) |
| `0x82F37798` | `D3DDevice_EndTiling` | high | (dev, ResolveFlags, pResolveRects, pDestTexture, pClearColor, ...); the destination is then passed to Swap (a resolve copy on PC) |
| `0x82F33D50` | `D3DDevice_Swap` | high | (dev, pFrontBuffer texture, pParameters); reaches `VdSwap`. The game presents with EndTiling + Swap, not Present |
| `0x82F2C640` | `D3DDevice_SetTexture` | high | (dev, Sampler n, pTexture, 0x80000000 >> n dirty bit) |
| `0x82F2A658`, `0x82F2A030` | `D3DDevice_SetRenderTarget` (wrapper, body) | high | (dev, RenderTargetIndex 0, pSurface) |
| `0x82F2A380` | `D3DDevice_SetDepthStencilSurface` | high | (dev, pSurface) |
| `0x82F2E658` | `D3DDevice_SetVertexShader` | high | binds objects built by `0x82F30050`; stores at device+0x3248 |
| `0x82F2E498` | `D3DDevice_SetPixelShader` | high | binds objects built by `0x82F2FF28`; stores at device+0x3244 |
| `0x82F2E828` (-> `0x82F2E840`) | `D3DDevice_SetVertexDeclaration` | likely | the third object setter, called with the others |
| `0x82F30050` | vertex shader construct | high | zeroes a 0x368-byte header, then copies the microcode `{pointer, size}` after it |
| `0x82F2FF28` | pixel shader construct | high | same with a 0x28-byte header |
| `0x82F2E100`, `0x82F2E1D8` | `SetVertexShaderConstantF` / `SetPixelShaderConstantF` (order unknown) | likely | (dev, StartRegister, pConstantData, Vector4fCount) |
| `0x82F29F00` | `D3DDevice_SetViewport` | likely | (dev, pViewport); called right after BeginTiling |
| `0x82F2C3E0` | `D3DDevice_CreateRenderTarget` | likely | (1296, 736, D3DFORMAT 0x1A220197 / 0x18280186, ...) |
| `0x82F2C2C0` | `D3DDevice_CreateTexture` | likely | (1280, 720, Levels 1, Usage 1, 0, D3DFORMAT 0x28280106, ...) |
| `0x82F27368`-`0x82F28BF8` | render state setters | high | called through the device's render state table; r6 = the setter, r7 = its table slot |
| `0x82F28C08`, `0x82F28DB0`, `0x82F28F50`, `0x82F29380`, `0x82F293D0`, ... | sampler state setters | likely | (dev, Sampler, Value) |

No `SetStreamSource` or `SetIndices` calls appear in traced frames; in this XDK they are
probably inline writes to the device's fetch constants, read back at draw time.

Still to identify: `Clear` (if used outside tiling), `SetScissorRect`, the vertex/index
buffer and texture create/lock functions (`0x82F2C048`, `0x82F2C518`, `0x82F29728`,
`0x82F22C20` are candidates), and where the stream/index fetch state lives.

## Device layout

The device is **0x5F00 bytes**, 0x100 more than Unleashed Recompiled's `GuestDevice`
(0x5E00), so this XDK version's layout differs and each field Unleashed reads has to be
located again. Found so far (offsets from the device):

| Offset | Field |
|-|-|
| `0x00`-`0x27` | dirty flags: five 64-bit words, flushed at the start of every draw |
| `0x28`-`0x188` | render state setter table, 89 entries (Unleashed's has 101 at `0x40`) |
| `0x0780`-`0x177F` | vertex shader float constants (256 float4), uploaded to ALU constant 0x4000 |
| `0x1780`-`0x277F` | pixel shader float constants (256 float4), uploaded to ALU constant 0x4400 |
| `0x2A90` | pointer to the GPU-written completed fence |
| `0x2A9C` | current fence |
| `0x35A4` / `0x35A8` | command buffer write pointer / limit |
| `0x3244` | current pixel shader |
| `0x3248` | current vertex shader |
| `0x5E80` | BehaviorFlags from CreateDevice |

## Runtime tracing

Configure with `-DNB_TRACE_D3D=ON` to wrap every function in `0x82F1C000-0x82F50000`
(`tools/gen_trace_wrappers.py`): calls from game code are logged with r3-r8 and f1, and
call counts are reported every 5 seconds. `NB_TRACE_FRAME=N` logs every call of frame N
(frames end at Swap). Tracing also enables the headless GPU
(`src/gpu/headless.cpp`), which completes fences, mirrors the ring buffer read pointer and
raises a 60 Hz vblank so the game runs its render loop without a GPU.

## Method

`tools/nbanalyse` has the tools used:

- `callgraph XEX begin end`: per function in a range, external/internal caller counts,
  imports used and PM4 type-3 packet headers built (`opcode/count`).
- `xrefs XEX addr...`: every branch to an address and the function it sits in.
- `funcinfo`, `disas`, `peek`, `imports`.

Anchors come from kernel calls (`Vd*`, `MmAllocatePhysicalMemoryEx`), PM4 opcodes
(e.g. `DRAW_INDX` 0x22, `DRAW_INDX_2` 0x36, `SET_CONSTANT` 0x2D, `EVENT_WRITE` 0x46)
and argument shapes matching the XDK API.
