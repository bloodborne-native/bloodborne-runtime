# Render adapter

[← Documentation](README.md) · [Native renderer](NATIVE_RENDERER.md)

The render adapter is the project's own GPU interface (`gpu/shadps4/video_core/renderer_backend/`).
The renderer asks it for resources, passes, pipelines and commands; a backend per graphics API
carries them out: Vulkan (Linux, and MoltenVK on macOS as a fallback) and Metal (macOS). It is
step N4 of the [native renderer](NATIVE_RENDERER.md); the Metal backend is N5. Status and next
steps are kept in [Native renderer](NATIVE_RENDERER.md#progress); this page is the specification.

**State.** A0–A6 and M1–M4 are implemented. The renderer records through the adapter; on Apple
Silicon the Metal backend is the default and owns its device, queue, resources, synchronization,
presentation and MetalFX without linking or loading the Vulkan loader or MoltenVK. M4 is
**accepted** (N5, the maintainer, 2026-10-29). MoltenVK stays selectable (`BB_METAL_FRAME=0`) until
the maintainer decides whether the fallback leaves the macOS app; only that path loads the Vulkan
loader and its driver. Linux keeps the Vulkan backend.

## Why

The goal is a native port: the game's code recompiled ahead of time, its shaders converted ahead
of time, and its graphics drawn by the project's own renderer. The renderer began as shadPS4's
Vulkan renderer. Every later step (N2 shaders, N3 textures, N6 the engine's draws) needs a
renderer API that is not shadPS4's Vulkan code, so the adapter is designed around what a renderer
needs; shadPS4's code is its first client, retired piece by piece. The cost: shadPS4 upstream
fixes no longer merge cleanly and are ported by hand, as bbport's already are.

| Part | With the adapter |
|---|---|
| Packet reader (`amdgpu/`), bbport's parallel draw preparation | Unchanged (until N6) |
| Shader recompiler (GCN → IR → SPIR-V) | Unchanged; its output is an adapter shader source |
| Texture and buffer caches (detiling, overlaps, invalidation, page tracking, the guest pool) | Logic unchanged; objects, copies and accesses through the adapter |
| Rasterizer (draw state, pipeline keys, draw preparation) | Records adapter commands; names no Vulkan type |
| Instance, device, queues, swapchain, VMA, descriptors, barriers | Inside the Vulkan backend |
| `mtl_frame`, `mtl_recorder`, `mtl_pso_cache`, `mtl_shader`, `mtl_resources`, `mtl_present`, the SPIR-V rewrites | The Metal backend (`renderer_metal/`) |

## Contract

### Objects

- One `Device` per process; the backend is chosen at start-up (`BB_GFX_BACKEND=vulkan|metal`,
  set by `run.sh`). No frame mixes backends.
- Opaque handles with a slot and a generation (`handle_table.h`, `resource.h`): buffers, images,
  views, samplers, shaders, graphics and compute pipelines, query pools. A stale handle is
  caught, not dereferenced.
- Objects are made from descriptions (`BufferDesc`, `ImageDesc`, `ImageViewDesc`, `SamplerDesc`,
  pipeline descriptions). `ImageDesc` says whether 3D images are viewed as 2D slices, compressed
  images as uncompressed (only where the device reports `block_texel_views`), and whether views
  in other formats are allowed (`mutable_format`).
- `Destroy` is deferred: the backend frees the object after the last submission that could use
  it has completed (`DeferRelease`). Callers never wait for the GPU to free something. An object
  used every frame (a view of an upscaler image) is kept with its owner, not made and freed per
  frame.
- The guest pool is imported from host memory (`ImportHostMemory`: Vulkan's external host
  memory, Metal's `newBufferWithBytesNoCopy`). The backend splits it where its API needs to
  (MoltenVK's 4 GiB offset limit is the Vulkan backend's concern).
- `Capabilities` hold the limits and features the renderer asks for (binding alignments,
  tessellation, geometry shaders, sample counts, query support, `memory_report`,
  `block_texel_views`...); `ReportMemory` gives usage, budget and total from one driver query.
  Answers asked per draw or per image (`CanBlit`, `SupportedSampleCounts`) are cached by the
  device.

### Vocabulary

The adapter has its own enums (`format.h`, `state.h`, `pass.h`): formats (153), blend factors and
operations, compare and stencil operations, topology, cull and winding, filters and address
modes, sample counts, usages, view types, aspects, swizzles. They keep Vulkan's numeric values,
checked at compile time (`vk_format.h`, `vk_state.h`), so pipeline keys and the shader cache stay
valid. No Vulkan header is needed to use them.

### Binding slots

Flat, typed slots per stage (`binding.h`: `SlotWrite` with a slot, count, kind and elements;
`BufferBinding`, `ImageBinding`), as Vulkan's push descriptors and Metal's direct slots: no
descriptor sets, tables or argument buffers. A draw's or dispatch's resources are bound with
`CommandRecorder::BindResources` (the pipeline's allocation, its slot writes and push constants).
The bindings share the layout of Vulkan's descriptor infos, so the Vulkan backend reads them in
place.

### Commands

`Device::Recorder()` gives the command stream (`commands.h`, `CommandRecorder`):

- **Passes:** `BeginPass`, `EndPass` and pass queries; attachments (`RenderAttachment`,
  `RenderState`) with load actions, so a deferred clear is the next pass's load action.
- **Draw state:** `DrawState` (viewports, scissors, depth, stencil, culling, depth bias, blend
  constants, write masks), tracked with its changes and recorded with `CommitDrawState`.
- **Draws:** direct, indexed, indirect and indirect-count; dispatch and indirect dispatch; vertex
  input (`SetVertexInput`), vertex and index buffers. Each recorded form captures only its own
  fields.
- **Transfers** (`transfers.h`): buffer and image copies, uploads and downloads, fills and
  updates, clears with their deferral, blits and depth resampling, copies placed before a pass
  (`CopyBufferBeforePass`), the staging pool; readbacks (`ReadBackAfter`).
- **Queries and diagnostics:** occlusion and timestamp queries, labels, breadcrumbs.

### Synchronization and lifetimes

The core declares how each command uses each resource (`ImageAccess`: sampled, storage, target,
feedback, depth read or written, compute, transfer; `BufferAccess`; `MemoryUse` for barriers) and
never names a layout, stage mask or barrier. The Vulkan backend turns declarations into barriers
and layouts (its per-image state lives in the backend's record of the image); the Metal backend
places encoder boundaries and fences. `TransitImage` moves a device-made image to a declared use.
Work that may not span passes or submissions registers as a `CarriedScope` (occlusion queries).

Submissions return ticks: `CurrentTick`, `CompletedTick`, `RefreshTicks`, `IsComplete`, `Wait`,
`WaitFor` (complete, timed out or device lost), and `AfterCompletion` to run work once the GPU is
past a tick.

### Shaders and pipelines

A shader is made from SPIR-V plus its binding layout (`CreateShader`; an empty handle when the
code is rejected). Pipelines are made from backend-neutral descriptions (`pipeline.h`: stages and
their shaders, binding slots and push constants, vertex input, topology, rasterization, sample
counts, colour targets and blending, depth and stencil formats). Guest pipelines also carry their
guest state (`GuestGraphics`, `GuestCompute`), from which the Metal backend builds its own
pipeline, emulating rect and quad lists, tessellation and geometry shaders. The renderer's own
passes (post-processing, motion vectors, the upscalers' helper passes, tiling) are made the same
way, marked `host_pass` (`host_pipeline.h`). A pipeline holds its shaders; a shader is freed
(deferred) when its caller and its last pipeline have released it. The Metal backend keeps built
pipeline states in a binary archive between sessions.

### Presentation and native interop

- `Swapchain` (`swapchain.h`): resize, acquire, present, image format, size and count, HDR; the
  present mode comes from the settings. The Metal backend presents into the window's
  `CAMetalLayer` with no Vulkan surface.
- `NativeHandle(object)` gives the backend's object to code that must call a vendor library:
  FidelityFX (FSR 3.1), FSR 4 and DLSS on the Vulkan backend, MetalFX and FSR 4.1.x
  (`vk_fsr4_metal`) on macOS, and the overlay's Dear ImGui renderer.

## Steps

Each step kept the renderer playable, kept Linux building, and was measured against the step
before it (M3 Pro, copied saves, alternating runs; "Iosefka's Clinic" is the test save's place).

| Step | What moved | Accepted with |
|---|---|---|
| A0 ✓ | `Device`, generation-checked handles, the Vulkan `BackendDevice` (instance, scheduler, deferred release) | No change on MoltenVK: 29.6 against 29.5 FPS, p90 35.0 against 35.1 ms |
| A1 ✓ | Own formats and pipeline-state enums in the caches and pipeline keys | Both caches warm: no Vulkan pipeline compiled, Metal's archive served every pipeline |
| A2 ✓ | Buffers, images, views and samplers made and freed through the adapter | Metal API validation clean; image memory and frame time unchanged |
| A3 ✓ | Ticks, copy regions, barrier state inside the backend | Validation clean; frames correct; 36.7 and 35.6 FPS against 35.9 and 36.3 |
| A4 ✓ | Access declarations, binding slots, dynamic state, render targets, draws and dispatches | GPU-thread recording 4.1 µs per draw against 4.2 (budget 5.6) |
| A5 ✓ | Shaders, vertex input and pipelines through the device; pipelines hold their shaders | Every pipeline builds (400 in a test run, 550 in a long session; 0 failed); 4.38 against 4.37 µs per draw |
| A6 ✓ | Resource binding, the renderer's own passes, frame images, swapchain, upscaler images | Every upscaler on this Mac renders (off, FSR 3.1, TAA, MetalFX); frame times as before |
| M1 ✓ | Buffers and images made in Metal | Loads a save and plays with `BB_GFX_BACKEND=metal`; 25.9 against 26.0 ms median |
| M2 ✓ | All shared recording through the adapter; the Metal recorder, Metal pipelines, presentation in Metal, submissions committed in Metal | Iosefka's Clinic, FSR 3.1 Balanced: 36.2 FPS against MoltenVK's 27.6; a 10-minute session (21,904 frames) and a 3 h 26 min play session without faults |
| M3 ✓ | Upscaler off by default; MetalFX the Metal backend's only upscaler (decision 5) | Iosefka's Clinic: native 1080p 39.0 FPS, MetalFX Balanced 33.5, MetalFX Quality 31.7, FSR 3.1 Balanced 36.2 |
| M4 | Metal by default on Apple Silicon; every Vulkan call that reached MoltenVK replaced; start-up without the loader or MoltenVK; MetalFX on the frame's command buffer | Implemented: Vulkan calls reaching MoltenVK during play from about 916,000 a second to none, then none at start-up; native start-up against the build before it, native 1080p: 36.9 against 36.8 FPS; 0 capability differences on the M3 Pro. Acceptance pending (above) |

## What was learnt

- **Binding writes must not be copied.** Converting slot writes into a temporary array and then
  copying them cost about 4% of frame rate; converting straight into the recording chunk costs
  nothing measurable.
- **Recorded commands capture only their own fields.** Capturing the whole 80-byte `DrawCommand`
  raised recording from about 4.2 to 5.8 µs per draw.
- **No driver queries per submission.** Asking `ReportMemory().available` on every guest
  submission cost 5% of the GPU thread's time per draw (4.49 against 4.24 µs); the
  `memory_report` capability and one query removed it.
- **Replace whole objects, not halves.** Skipping only MoltenVK's command-buffer begin and end
  was slower in both orders (38.6 against 39.5 and 38.2 against 39.4 FPS, cause unknown); making
  the whole command buffer the Metal backend's own handle was not (39.1 against 38.8).
- **Images that allow other-format views cost Metal.** Made mutable, the upscaler's images cost
  MetalFX about 2.5% (30.9 against 30.1 ms median): Metal may leave such images uncompressed.
- **MetalFX belongs on the frame's command buffer.** Encoded natively it measured 34.5 mean FPS
  against 30.8 through the earlier export bridge (one pair), which copied colour, depth and
  motion through buffers on a second queue and waited on the CPU.
- **Metal has no uncompressed view of a compressed texture**, and no 2D view of a 3D texture. The
  game uses one 3D texture, viewed only as 3D; a 2D view of one would be logged.

## What remains in shared code

- **Vulkan types and entry points.** The shared renderer still uses Vulkan's types and calls some
  Vulkan entry points (instance, device, queue, capabilities, object creation). On the Metal path
  the Metal backend answers them (`mtl_vk_layer`, decision 6); capabilities come from a recorded
  profile plus the MTLDevice (`mtl_vk_caps`). There is one `Device` implementation,
  `BackendDevice`; with `BB_GFX_BACKEND=metal` its resources, recorder, pipelines and
  presentation are the Metal backend's. Removing Vulkan types from shared code can follow piece
  by piece.
- **The Vulkan backend's own parts.** VMA, the sparse guest arena and its fault manager and the
  guest-memory chunks used in place (Linux), the readback queue, the Vulkan presenter and the
  temporal upscaler class with FidelityFX, FSR 4 and DLSS.
- **The MoltenVK fallback on macOS** (`BB_METAL_FRAME=0`), with FSR 3.1 and FSR 4.1.x, until M4
  is accepted.

## Decisions

Taken by the maintainer. The numbers are cited in code comments; they do not change.

1. **The route.** The adapter replaces the earlier interception of the renderer's Vulkan calls.
   shadPS4 upstream fixes are ported by hand.
2. **Vocabulary.** Own enums with Vulkan's numeric values, so the shader and pipeline caches stay
   valid.
3. **One backend per process.** Chosen at start-up; no mixed-backend frame.
4. **MoltenVK stays as a fallback** on macOS, selectable with `BB_METAL_FRAME=0`, until M4 is
   accepted. (This replaced the plan to remove it with M4.)
5. **Upscalers by backend.** The Metal backend offers MetalFX for FSR 3.1, FSR 4.0.2 and DLSS
   (`ConfigureUpscalerSupport`'s `metalfx_only`), and the native FSR 4.1.x modes themselves:
   `vk_fsr4_metal` encodes them on the frame's own command buffer, reaching the frame's
   MTLTextures through the Metal layer's direct accessors. The MoltenVK fallback also offers
   FSR 4.1.x, through `VK_EXT_metal_objects`. FSR 3.1 and DLSS otherwise belong to the Vulkan
   backend. The upscaler is off by default.
6. **How Vulkan leaves the Metal path.** The Metal backend answers the Vulkan entry points the
   shared renderer calls, so neither the Vulkan loader nor MoltenVK is linked or loaded on the
   native path, while Vulkan's headers stay the shared code's vocabulary (Vulkan is the backend
   on the other platforms).
7. **No block-texel views on Metal.** The Metal backend refuses images that allow uncompressed
   views of block-compressed formats and reports no `block_texel_views`; the texture cache then
   keeps a compressed image and an uncompressed one apart, as for any incompatible formats
   (`TextureCache::CanView`, commit `82051e5`). Devices with such views (Vulkan) behave as before.
