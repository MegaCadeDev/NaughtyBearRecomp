# Naughty Bear's Direct3D library and the renderer

Naughty Bear links the Xbox 360 XDK Direct3D library statically. Unlike Unleashed
Recompiled, which replaces Sonic's Direct3D outright, this port lets the game's Direct3D
run (against the headless GPU, `src/gpu/headless.cpp`) and mirrors what it does: a few
functions are hooked by address (draws, tiling, resolves, swap, shader creation), and at
each draw the renderer reads the GPU state the device keeps (register images, fetch
constants, bound surfaces) and replays the draw with Vulkan. This page lists what was
found in the executable and how the renderer (`src/gpu/video.cpp`) uses it.

## Extent

All PM4 packet construction lives in `0x82F20000-0x82F50000`; that range is the D3D
library (Sonic Unleashed's is at `0x82BD9000-0x82C01000`). The game's own renderer
layer calls into it from relatively few places (at most 38 call sites per function),
so the number of hooks needed is manageable.

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
| `0x82F37EF0` | `D3DDevice_BeginVertices` | high | (dev, PrimitiveType, VertexCount, Stride) -> pointer into the command buffer; particles fill it, then `0x82F37EE0` (`EndVertices`) |
| `0x82F383D0` | `D3DDevice_BeginIndexedVertices` | high | (dev, PrimitiveType, -MinVertexIndex, NumVertices, IndexCount, IndexDataFormat, Stride, ppIndexData, ppVertexData on the stack) -> HRESULT |
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
| `0x82F29778` | `D3DDevice_SetStreamSource` | high | (dev, Stream, pVertexBuffer, Offset, Stride, dirty bit): writes vertex fetch constant 95 - Stream, keeps the buffer at device+12636+4n and stride / 4 at device+12704+n |
| `0x82F29898` | `D3DDevice_SetIndices` | high | (dev, pIndexBuffer): kept at device+12612 |
| `0x82F29630` | `D3DDevice_SetScissorRect` | high | (dev, pRect): writes the scissor into register images 0x2011/0x2012 |
| `0x82F35788` | `D3DDevice_Clear` | high | (dev, Count, pRects, Flags, Color, Z in f1, Stencil in r9); converts the colour and calls `ClearF` (`0x82F35660`, also used by BeginTiling) per rectangle. Flags: render targets 0-3 in bits 0-3, depth `0x10`, stencil `0x20`; rectangles are clipped to the viewport and the scissor. Scaleform clears the stencil with it before each mask |
| `0x82F398F0` | `D3DDevice_Resolve` | high | (dev, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace, pClearColor, ClearZ, ClearStencil, pParameters) |
| `0x82F36FC8` | `D3DDevice_SetPredication` | high | (dev, Mask): writes PM4 `SET_BIN_SELECT`; used with tiling, not a clear |
| `0x82F37EE0` | `D3DDevice_EndVertices` | high | ends a BeginVertices / BeginIndexedVertices draw |
| `0x82F47D40` | register flush (internal) | high | (dev, dirty bits, first register, device source): writes one register per set bit from consecutive device dwords |
| `0x82F27368`-`0x82F28BF8` | render state setters | high | called through the device's render state table; r6 = the setter, r7 = its table slot |
| `0x82F28C08`, `0x82F28DB0`, `0x82F28F50`, `0x82F29380`, `0x82F293D0`, ... | sampler state setters | likely | (dev, Sampler, Value) |

Not hooked yet: whatever builds the vertex shader objects used by a few depth-only draws
(they don't pass through the constructors above, so their shaders are unknown).

## Device layout

The device is **0x5F00 bytes**, 0x100 more than Unleashed Recompiled's `GuestDevice`
(0x5E00), so its fields had to be located again. The device keeps the GPU registers it
programs as images, one dword per register, which the draw functions flush to the command
buffer by dirty bits (`0x82F47D40`); the renderer reads them directly.

| Offset | Field |
|-|-|
| `0x00`-`0x27` | dirty flags: five 64-bit words, flushed at the start of every draw |
| `0x28`-`0x188` | render state setter table, 89 entries (Unleashed's has 101 at `0x40`) |
| `0x0480`-`0x077F` | fetch constants, 32 slots of 6 dwords (GPU registers 0x4800+): texture slots 0-15, vertex streams use vertex fetch constant 95 - n |
| `0x0780`-`0x177F` | vertex shader float constants (256 float4), uploaded to ALU constant 0x4000 |
| `0x1780`-`0x277F` | pixel shader float constants (256 float4), uploaded to ALU constant 0x4400 |
| `10112` | boolean constants (registers 0x4900+): vertex shader bools, pixel shader bools at +16 |
| `10368` | register image 0x2000-0x2012 (`RB_SURFACE_INFO`, `RB_COLOR_INFO`, `RB_DEPTH_INFO`, ..., scissor at 0x2011/0x2012) |
| `10444` | register image 0x2100-0x2114 (`RB_COLOR_MASK`, stencil reference, `RB_ALPHA_REF`, `PA_CL_VPORT_*`) |
| `10528` | register image 0x2180-0x2184 |
| `10548` | register image 0x2200-0x220B (`RB_DEPTHCONTROL`, `RB_BLENDCONTROL0-3`, `RB_COLORCONTROL`, `PA_SU_SC_MODE_CNTL`, `PA_CL_VTE_CNTL`) |
| `10596`, `10680` | register images 0x2280-0x2294, 0x2300-0x2325 |
| `0x2A90` | pointer to the GPU-written completed fence |
| `0x2A9C` | current fence |
| `0x2ED8` | current vertex declaration (element count at +24, 12-byte `D3DVERTEXELEMENT9` from +52) |
| `0x3244` / `0x3248` | current pixel / vertex shader |
| `12612` | current index buffer |
| `12032` | scissor test enabled |
| `12616`-`12628`, `12632` | render targets 0-3, depth/stencil surface |
| `12636` + 4n, `12704` + n | vertex buffer and stride / 4 of stream n |
| `12824` | viewport (`X, Y, Width, Height, MinZ, MaxZ` as floats) |
| `12852` | scissor rectangle (left, top, right, bottom) |
| `0x35A4` / `0x35A8` | command buffer write pointer / limit |
| `0x5E80` | BehaviorFlags from CreateDevice |

Objects: surfaces keep their `RB_SURFACE_INFO` image (EDRAM pitch) at +24, their
`RB_COLOR_INFO`/`RB_DEPTH_INFO` at +28 and their EDRAM size at +44; textures their fetch
constant at +0x1C; vertex and index buffers their address at +24 (index buffers flag
32-bit indices in the sign bit of their first dword).

## The renderer

At each draw `src/gpu/video.cpp`:

- looks up the bound shaders (by the hash recorded when they were created) in the
  translated shader cache;
- binds host render targets for the device's surfaces: every guest surface gets a host
  texture, sized from its EDRAM pitch and size; depth/stencil is `D32_FLOAT_S8_UINT`;
- builds the pipeline state from the register images: blend (`RB_BLENDCONTROL0`), colour
  mask, depth and stencil (`RB_DEPTHCONTROL`, `RB_STENCILREFMASK`), alpha test
  (`RB_COLORCONTROL`, a specialization constant in the translated shaders), viewport
  (`PA_CL_VPORT_*`, keeping the reversed depth range the game uses), scissor;
- decodes the vertex streams named by the vertex declaration from their fetch constants:
  every element, of any `D3DDECLTYPE`, becomes a float4 (or uint4 for blend indices and
  the normal-type inputs, which XenosRecomp reads as raw bits); converted buffers are
  cached by content, and small streams (bone palettes) are decoded per draw. Each input
  location's stream is also passed to the shaders by address, for vertex fetches indexed
  by a computed register (skinning palettes, particles);
- converts strips, fans and quads to triangle lists on the CPU (honouring strip restarts);
- reads textures from their fetch constants (untiling and byte-swapping them), and their
  samplers' clamp and filter modes. Uploaded textures are kept, and their guest memory is
  hashed again when they're used in a later frame (small ones every frame, large ones
  every 16): the game loads each menu's and loading screen's images into memory earlier
  ones used, so a texture at the same address with the same format can be a new image.

Draws come from `DrawVertices`, `DrawIndexedVertices` and the `Begin(Indexed)Vertices` /
`EndVertices` pair (which `DrawVerticesUP` and `DrawIndexedVerticesUP` use too); particle
systems call the latter directly.

`Clear` clears the bound targets within its rectangles; the menus and HUD (Scaleform)
clear the stencil before drawing each mask shape, then draw the masked content with a
stencil test.

The game renders with predicated tiling: `BeginTiling` clears the targets, `EndTiling`
and `Resolve` copy a surface into a texture. Resolves copy into a host texture that stands
in for the destination guest texture (by its physical address) when it is sampled later;
depth resolves become a float texture. `Swap` presents the resolved front buffer: it's
copied to a 1280x720 target and on to the swap chain. The copies use small GLSL shaders
(`src/gpu/shaders/`), compiled at build time.

## Runtime tracing

Configure with `-DNB_TRACE_D3D=ON` to wrap every function in `0x82F1C000-0x82F50000`
(`tools/gen_trace_wrappers.py`): calls from game code are logged with r3-r8 and f1, and
call counts are reported every 5 seconds. `NB_TRACE_FRAME=N` logs every call of frame N
(frames end at Swap).

## Method

`tools/nbanalyse` has the tools used:

- `callgraph XEX begin end`: per function in a range, external/internal caller counts,
  imports used and PM4 type-3 packet headers built (`opcode/count`).
- `xrefs XEX addr...`: every branch to an address and the function it sits in.
- `funcinfo`, `disas`, `peek`, `imports`.

Anchors come from kernel calls (`Vd*`, `MmAllocatePhysicalMemoryEx`), PM4 opcodes
(e.g. `DRAW_INDX` 0x22, `DRAW_INDX_2` 0x36, `SET_CONSTANT` 0x2D, `EVENT_WRITE` 0x46)
and argument shapes matching the XDK API.

## Shaders

The game's data is in packages (`lu/*.lu`): a header, then the payload, stored as is or
as 1 MB chunks compressed with XMemCompress (LZX). `src/kernel/packages.cpp` reads them,
decompressing with the game's own recompiled `XMemCreateDecompressionContext`
(`0x830B5480`, LZX, 1 MB window, 64 KB partitions) and `XMemDecompress` (`0x830B5458`).
These use the game's allocator, so they run once the game is up (after CreateDevice).

The packages and the executable hold 508 distinct shader containers (standard XDK
containers: magic `0x102A1100`/`0x102A1101`, `virtualSize + physicalSize` bytes). About 30
more are built in memory at boot. `src/gpu/shader_store.cpp` translates all of them on the
first run and keeps the SPIR-V in `cache/shaders.bin`; shaders are identified by the XXH3
hash of their container, recorded when the game creates them (constructors
`0x82F30050`/`0x82F2FF28`, `CreateVertexShader`/`CreatePixelShader` `0x82F30188`/
`0x82F2FF60`), and any one not yet translated is translated right there.

Patches to XenosRecomp (`patches/XenosRecomp/`) for Naughty Bear:

- Extra vertex input locations: 16-18 for `BLENDWEIGHT1`-`3`, 19-21 for
  `BLENDINDICES1`-`3`, 22-24 for the second normal/tangent/binormal and 25-26 for
  `COLOR2`-`3` (the runtime's table in `src/gpu/video.cpp` must match).
- Inputs a shader fetches more than once are declared once; fetches indexed by a computed
  register (bone palettes, particles) read the stream directly (`vfetchIndexed`).
- Vertex shaders start with the vertex index in r0.x, as on the GPU (particles build their
  quads from it).
- Boolean constant arrays, with pixel shader boolean registers addressed from 128.
- A shader that fails to compile is reported and left out instead of aborting.

`tools/translate_shaders.sh` still translates a folder of dumped containers
(`NB_DUMP_SHADERS=dir`) offline, for debugging the translator.
