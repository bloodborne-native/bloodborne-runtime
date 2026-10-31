# Native renderer (milestone R5)

[← Documentation](README.md)

**Goal:** the game's graphics drawn with each platform's own GPU API, with nothing translated
while the game runs: no MoltenVK on macOS, no shaders compiled during play. The renderer stays
multiplatform: one shared core, and thin backends for Vulkan (Linux, Windows) and Metal (macOS).
It is the graphics half of the native port; the CPU half is the
[recompiler](https://github.com/bloodborne-native/bloodborne-recomp/blob/main/docs/RECOMPILATION.md).

This page is the renderer's status and plan. The GPU interface it is built on is specified in
[Render adapter](RENDER_ADAPTER.md); what a frame costs is in [Performance](PERFORMANCE.md).

## Principles

1. **Multiplatform.** Nothing works on Metal only. Planning a frame, tracking resources and
   converting shaders happen in the shared core; a backend only speaks its API.
2. **Always playable.** Native Metal is the default on Apple Silicon, accepted (N5, 2026-10-29); the
   MoltenVK renderer stays selectable (`BB_METAL_FRAME=0`) until the maintainer decides whether the
   fallback leaves the macOS app.
3. **Measured.** Each step is compared against the path it replaces in alternating runs: FPS,
   frame-time p90, CPU time, the GPU thread's time per draw, and GPU time from Metal System
   Traces.
4. **Ahead of time where possible.** Shaders and texture layouts converted once, on the player's
   machine, from their own copy of the game. No game code, shaders or assets in any repository.

## Progress

Each bar has 20 blocks, one per 5% of the step's accepted scope. R5 is the mean of its seven
steps.

| Step | Progress | State, and the next action |
|---|---|---|
| **R5 · native renderer** | 80% `████████████████░░░░` | N0, N1, N4, N5 and N6 done; N2 at 75% (N2.3 done; N2.4 at 80%: 92% of draw stages and 79% of dispatch stages without the walker, opt-in) |
| N0 · scope | 100% `████████████████████` | Done: where the commands come from and where a frame's time goes |
| N1 · frame planning | 100% `████████████████████` | Done: clears deferred into the next pass's load action |
| N2 · shaders from the draw state, then ahead of time | 75% `███████████████░░░░░` | 3 of 5 steps and most of N2.4: 92% of draw shader stages and 79% of dispatch stages run without the PS4 user data's walker (opt-in). Next: the rest of N2.4 (programs without a plan, surface operations and draws that walk tables) and the shaders ahead of time (N2.5) |
| N3 · textures as objects | 0% `░░░░░░░░░░░░░░░░░░░░` | After N2. First action: trace where the engine creates and releases textures |
| N4 · render adapter | 100% `████████████████████` | Done: A0–A6; the renderer records through the [adapter](RENDER_ADAPTER.md) |
| N5 · Metal backend | 100% `████████████████████` | Accepted (the maintainer, 2026-10-29): M1–M4, native Metal the default since 0.9.0 RC 1 |
| N6 · engine render boundary | 100% `████████████████████` | Done (re-scoped on 2026-10-28): the boundary found and observed, every input of a draw found from the draw state, mesh draws bound from it ([below](#n6-the-engines-render-boundary)); taking the other draw kinds over moved to N2 |

## Now and next

In this order (the app's roadmap):

1. **N5 accepted** (the maintainer, 2026-10-29): native Metal has been the default since 0.9.0
   RC 1, through the maintainer's play sessions and players' reports on the 0.9.x releases. The
   app still bundles MoltenVK and the Vulkan loader for the fallback (`BB_METAL_FRAME=0`); whether
   it leaves the macOS app is the maintainer's decision.
2. **N2, shaders from the draw state, then native shaders ahead of time (now: the maintainer's
   focus from 2026-10-29).** N6 found that each resource a shader reads comes from a fixed slot of
   the engine's draw state. While shaders are converted GCN code they read the PS4 user data
   themselves, so nothing is saved until they are built to read the draw state's slots instead: N2
   does that, mesh draws first, then the other draw kinds, then the game's shaders built once
   ahead of time on the player's device, as native shaders for each platform (Metal libraries on
   macOS and iOS, SPIR-V for Vulkan). Its steps: [below](#n2-shaders-from-the-draw-state-then-ahead-of-time).
3. **N3, textures as objects.** Textures are still inferred from GPU memory by shadPS4's texture
   cache, and each one lives twice: in the game's memory and as a Metal texture. Taking them as
   the objects the engine creates and releases removes the copies (about 1–1.5 GB in Cathedral
   Ward), which smaller Macs and the iPhone need.
4. **The recompiler's CPU work** (bloodborne-recomp): calls left to the translator are down from
   69,600 a second to 9,400; what is left is memory ordering kept only around the engine's
   synchronization, with the maintainer's conditions.

## What was measured

- **The command stream (N0).** Bloodborne links libGnm statically: the game writes the PS4's
  command packets (PM4) itself, through 99 known libGnm methods, and the packets record those
  calls without loss. Until N6, the packet reader is the front end.
- **A frame on MoltenVK (N0;** Hunter's Dream, about 48 FPS): 14.9 ms of GPU
  work and 5.9 ms of GPU idle (28% of the frame) in about 420 Metal encoders. Every clear,
  tessellated draw (MoltenVK runs the vertex and hull stages as a compute pass) and copy between
  draws ended a render pass. CPU: the game's code about 3.8 cores, the renderer about 1.2,
  MoltenVK and Metal about 0.6.
- **Frame planning (N1).** Eligible clears become the next pass's load action (on by default;
  `BB_DEFER_CLEARS=0` turns it off): +1–2 FPS. Drawing fixed-factor tessellation as triangles
  (`BB_FIXED_QUAD_TRIANGLES=1`) cost more CPU and p90 than it saved and stays opt-in; Metal's own
  tessellator does that work in the Metal backend. Grouping copies gained nothing.
- **The Metal backend (N5)**, M3 Pro, 1080p output:

  | Where, setting | Metal | MoltenVK |
  |---|---|---|
  | Iosefka's Clinic (the test save), native 1080p | 38.2–39.6 FPS | 30.6 FPS |
  | Hunter's Dream, FSR 3.1 Balanced (the Metal backend's first version, when it still offered FSR) | 65.5 and 61.7 FPS, p90 16.3 and 17.6 ms | 58.0 and 58.1 FPS, p90 20.4 and 18.9 ms |
  | A new game, 3 h 26 min of play by the maintainer, native 1080p | median 42.2 FPS, 37.2 or more 90% of the time, no faults | not measured |

  Every pipeline the game used in a long session builds in Metal (550: 510 graphics, 40 compute;
  0 failed), and no draw met a missing pipeline or an unsupported command. The GPU thread
  records a draw in about 4.0–4.4 µs on the Metal backend.
- **Upscalers on Metal.** At native 1080p no upscaler beats native: in Iosefka's Clinic native
  39.0 FPS, MetalFX Balanced 33.5, MetalFX Quality 31.7. At 1080p the frame is not bound by
  pixel work, so the upscaler is off by default.

## The engine's render boundary (for N6)

Dantelion2's CoreGraphics2 draws through seven methods of its graphics context (`draw`,
`drawInstanced`, `drawIndexed`, `drawIndexedInstanced`, `drawIndirect`, `drawIndexedIndirect`,
`dispatch`). Each applies the state blocks of the draw state it is given, binds the shader
resources and calls libGnm. Counted with `BB_DRAW_CENSUS=1` (M3 Pro, translator, the player's
save in a level, 33 FPS: about 62,000 libGnm draws and dispatches a second, about 1,880 a frame):

| Through | Share of libGnm's draws and dispatches |
|---|---:|
| The seven methods (`drawIndexed` 33,000 a second, `draw` 17,800, `drawIndexedInstanced` 1,800, `drawIndirect` 100) | 85% |
| CoreGraphics2's compute passes: buffer copies and fills, texture clears and copies, on the same context and resource binder | 7% |
| Render-target passes (fast-clear elimination, depth-stencil clears, depth decompression) | 2% |
| Scaleform's renderer (menus and HUD) and one small wrapper outside CoreGraphics2 | 6% |

The compute passes make about 4,400 dispatches a second, which is nearly every dispatch: the
only others are the few the render-target passes make. `dispatch`, the seventh method, did not
run in the count.

So N6 can take nearly all of the world's draws at the seven methods; the compute passes, the
render-target passes and Scaleform are three smaller boundaries of their own.

**Who calls the seven methods** (the 16 busiest call sites): 65% of the calls come from
CoreGraphics2's mesh draw (`CG::MeshRenderer::drawMesh`: one draw per face set of a mesh, with
the mesh's vertex buffers and input layout); then three functions with 18%, 7.5% and 5.9%. Every
mesh draw counted used one material class, which picks a shader program by technique and pass
type and binds its five stages' shaders, its constants (parameter objects writing its constant
buffer) and its textures (pixel stage, and domain stage for tessellation). The 18% and the 5.9%
are deferred lighting (batches of up to 12 lights drawn as full-screen quads limited by the
batch's depth bounds); the 7.5% draws geometry written into an upload ring.

**The draw state** is one object modelled on Direct3D 11: input assembly, 32 vertex buffers, the
rasterizer, blend and depth-stencil states, eight colour targets and a depth target, and five
graphics stages, each with 16 samplers, 128 textures and 14 constant buffers. Its known layout is
in bloodborne-recomp's `annotations/structs.h`.

## N6: the engine's render boundary

R4 (bloodborne-recomp) named the boundary and described its objects:
[the annotations](https://github.com/bloodborne-native/bloodborne-recomp/blob/main/docs/RECOMPILATION.md#annotations),
with the draw state's layout in `annotations/structs.h`.

### Steps

| Step | Progress | State |
|---|---|---|
| N6.1 · the plan | 100% `████████████████████` | This section |
| N6.2 · draws observed | 100% `████████████████████` | Every draw through the methods matched to its packets; topology, counts, index buffer and shaders agree |
| N6.3 · output state observed | 100% `████████████████████` | Render and depth targets, blend, depth-stencil, rasterizer, viewport, scissor, stencil reference, blend colour and depth bounds agree |
| N6.4 · resources observed | 100% `████████████████████` | The textures, samplers, constant buffers and vertex buffers the shaders read are the draw state's |
| N6.5 · mesh draws from the draw state | 100% `████████████████████` | `CG::MeshRenderer::drawMesh`'s draws (65% of the methods' calls): the buffers' addresses from the engine's managers; each shader resource's slot in the draw state; their resources bound from the record behind `BB_RENDER_BOUNDARY=mesh` |

N6 was re-scoped on 2026-10-28 (maintainer). Its last steps were to skip the draw-preparation
workers' user-data walk for mesh draws and then take the other draw kinds over the same way. But
converted GCN shaders read the user data themselves (the renderer uploads it with the flattened
user data for every draw), so taking draws over saves nothing while the shaders are GCN code. The
other draw kinds (the deferred lighting's quads, the upload ring's geometry; compute passes,
render-target passes including the methods' own fast-clear resolves, and Scaleform) move to N2,
which builds their shaders to read the draw state instead.

### How a draw reaches the renderer

- **Tags in the stream.** An entry hook on each draw method reads the draw state it is given into
  a record (`gpu/shim/bbport_render_boundary.cpp`). A hook on libGnm's draw functions files the
  record under a number and writes a three-dword PM4 NOP with that number just before the draw's
  packet, for the method's own draw only (the libGnm call returning into the method): a method
  that changes a render target first resolves the old one's fast clear with a draw of its own,
  which keeps the packet path. The GPU ignores the NOP; the engine copies its recorded commands into the buffers it
  submits, and the tag travels with them. The packet reader, meeting the tag and then the draw,
  knows which record the draw is.
- **One stream, one order.** A draw taken over is still recorded where its packet stands in the
  stream: the reader hands the record to the native renderer at the tag instead of decoding the
  registers. Waits, labels, flips, compute passes and every draw not taken over keep the packet
  path in the same stream, so nothing needs reordering or extra synchronization.
- **Translator and recompiled library.** The hooks rewrite the methods' first instructions, so
  those 11 functions fail the recompiled library's byte check and run translated; everything they
  call stays recompiled. The cost is measured with each step.

### How the draw state maps onto the adapter

- **Pipeline.** The five stages' shaders (each `CgShader`'s code address) are still GCN code,
  recompiled and converted as today (N2 changes that); the input layout and the vertex strides
  give the vertex input; topology, the rasterizer, blend and depth-stencil blocks and the render
  and depth targets' formats give the rest of the pipeline description.
- **Draw state.** Viewports, scissors, the blend colour, depth bounds and the stencil reference
  map onto the adapter's `DrawState`.
- **Bindings.** Each stage's samplers (S#), textures (T#) and constant buffers (V#s made from
  address and size) are the resources the engine's binder copies into its descriptor tables.
  Each resource a shader reads comes from a fixed slot of the draw state: per shader program
  for textures, samplers and constant buffers, per vertex program and input layout for vertex
  attributes. The observer learns these slot maps from the draws it sees.
- **Vertex and index buffers.** 32 vertex buffers with strides and offsets; the index buffer with
  its DXGI format and byte offset. The shader reads one V# per vertex attribute: the buffer's
  address plus the attribute's offset in a vertex, with the buffer's stride.
- **Buffer addresses.** Constant buffers, and the index and vertex buffers marked `managed`, are
  placed by the context's managers, which look their GPU addresses up in tables of their own:
  constant buffers through the manager at context + 0xb3c0 (`0x26ae840`), index and vertex
  buffers through the one at context + 0xb6f8 (`0x26d0ff0`), each given the buffer's handle at
  +0x18. The libGnm draw hook asks them (`runtime_guest_call` on the game's thread), after the
  binder has run, so the addresses are this draw's.

### What the observer found (N6.2 to N6.4)

`BB_RENDER_BOUNDARY=observe` (not with `BB_DRAW_CENSUS=1`) compares each tagged draw with the
registers the packet reader decoded, every 5 seconds in the log. In a two-minute run (M3 Pro,
the test save, translator, no faults):

- 3,006,657 draws went through the methods and were tagged; 3,004,963 (99.94%) reached the reader
  with their tag. The 311,385 draw packets without one are the 295,520 libGnm draws made outside
  the methods and the 15,906 fast-clear resolves the methods make themselves.
- Every field compared agrees in every tagged draw: topology, counts, index type and start,
  instances; the five stages' shaders; the colour and depth targets, the target mask, blend,
  depth-stencil and stencil operations, clip and primitive setup, viewport 0, the stencil
  reference, blend colour and depth bounds. The draw state's scissor applies only when its
  rasterizer block enables the viewport scissor (`scan_mode[1]`, 738,110 draws); then it agrees
  too. So for these draws the draw state alone says what the packets say.
- The resources (2,839,214 draws whose resources the draw-preparation workers had read in time,
  91%): every texture the shaders read is in the draw state's tables with the same T#
  (16,422,346); samplers 99.98% (9,392,182 of 9,394,165; the rest are most likely empty slots the
  binder fills with the engine's default sampler, not checked yet); every constant buffer matches
  one of the stage's by size (23,447,710); every vertex buffer by stride (12,134,315), and by
  address where the draw state places the buffers itself (11,721,677). The one buffer more per
  draw is shadPS4's null buffer, not the game's.
- The buffer addresses (N6.5, 2,606,707 prepared draws, no faults): with the managers' answers
  every constant buffer (21,497,235), every vertex buffer (11,121,153) and every indexed draw's
  first index (1,856,692) is at the address the draw state gives. Asking the managers on every
  draw costs the game's threads time (31.0 FPS in that run).
- The slot maps (N6.5, 218 programs and vertex layouts): every vertex attribute (368) and every
  texture and constant buffer read comes from a fixed slot. 373 textures and 475 constant buffers
  have exactly one; 57 textures and 72 buffers (shadPS4's null buffer among them) several slots
  that always held the same value, so any of them gives the right resource. Samplers are often
  identical across slots (50 one, 159 several); one fits no slot, most likely the engine's default
  sampler for an empty slot (not checked yet).
- Mesh draws from the draw state (N6.5, `BB_RENDER_BOUNDARY=mesh`): the textures, samplers,
  constant buffers and vertex buffers of every mesh draw whose slot maps are known are replaced,
  before the draw is recorded, by the ones the draw state names (T#s and S#s read from the
  engine's objects, buffer addresses from its managers, each vertex attribute at its slot's
  address plus its learnt offset). In two minutes 1,941,705 mesh draws took 50,453,065 resources
  this way, none differing from the user data's; 35.7 FPS, no faults. Nothing is saved yet: the
  workers still walk the user data and the hooks add their own cost.
- Observing costs frame rate: 34.2–34.9 FPS with every comparison, 37.4–37.6 with the first
  ones, 36.2–37.0 without it that day. It is a diagnostic only.

## N2: shaders from the draw state, then ahead of time

### Steps

| Step | Progress | State |
|---|---|---|
| N2.1 · the plan | 100% `████████████████████` | This section |
| N2.2 · the inventory | 100% `████████████████████` | The shaders use the user data only to locate their resources (below) |
| N2.3 · mesh draws from the draw state | 100% `████████████████████` | Done, opt-in (`BB_RENDER_BOUNDARY=flat`): mesh draws' flattened user data from the draw state on the workers and the GPU thread, without the walker; exact; the observer still costs 42 CPU points ([below](#n23s-design-the-flat-buffer-from-the-draw-state)) |
| N2.4 · the other draw kinds | 80% `████████████████░░░░` | The draw methods' other draws: done. Compute passes and the draws outside the methods: most read their sharps straight from the user-data registers, which needs no boundary ([below](#n24-the-other-draw-kinds-and-compute-passes)). Left: method draws whose program has no plan (6% of draw stages), the draws (2%) and surface operations (16% of dispatch stages) that walk tables |
| N2.5 · ahead of time | 40% `████████░░░░░░░░░░░░` | Designed and measured ([below](#n25-shaders-ahead-of-time)): a true first play, with Metal's own system cache empty too, spends 8.4 s compiling MSL into libraries and 4.9 s building pipelines; the worst frame takes 4.9 s. Shaders compiling on worker threads and translated ahead of the GPU thread (opt-in) bring the stalls from 12 s to 7 s and the worst frame to 2.0 s. Next: a long session with `BB_SHADER_AHEAD=1` before it becomes the default, then the pipelines' 4.9 s, which need keys the shaders do not hold |

### The inventory (N2.2)

`BB_RENDER_BOUNDARY=observe` also keeps, per shader program, how its resources reach it (the
shader recompiler's record of where each sharp's dwords come from), and with
`BB_RENDER_BOUNDARY_INVENTORY=path` writes the counts per program to a CSV file. In two minutes in
the test save's area (M3 Pro, 5,715,048 stage draws of prepared draws; tessellated and
geometry-shader draws are not prepared by the workers and not counted):

- 144 programs: 55 vertex, 89 pixel. 83 are used by mesh draws; 34 make 90% of the stage draws.
- Their 1,132 sharps come from user-data registers (266) or from tables in memory the binder
  wrote (866); none is built into a shader.
- No program reads a user-data register for anything but its resources, and none reads
  constants from the flattened user data. The only other buffer is the renderer's own (user clip
  planes, in every vertex program).
- At most 14 textures per program; at most 180 dwords of flattened user data.

So a shader rebuilt with fixed bindings for the draw-state slots its resources come from (N6's
slot maps) needs no user data at all: neither the walk nor the upload.

### N2.3's design: the flat buffer from the draw state

Where the cost is: for every stage of every draw, the draw-preparation worker runs the shader's
SRT walker (`Info::RefreshFlatBuf`, `RunSrtProgram`: about a quarter of the workers' time), which
follows the PS4 user data through the game's memory and writes each resource's sharp into the
flattened user data. Everything after reads that buffer: the shader's specialization (which
permutation the draw uses), `PrepareResources` (the sharps the GPU thread binds) and the shader
itself.

The change: for a mesh draw whose shader programs have complete slot maps (each resource from
exactly one draw-state slot), the worker writes the sharps from the engine's draw state into the
flattened buffer at the offsets the shader recompiler recorded (`sharp_idx`), after the
user-data registers, and does not run the walker. What reads the buffer is unchanged.

The steps:

1. **The worker knows its draw's record.** It already reads every packet of the submission
   (`ForEachPacket`); it notes the render boundary's NOP tag before a draw, as the packet reader
   does, and looks the record up.
2. **Slot maps the workers can read.** They are learned on the GPU thread; a program's map is
   frozen once every resource has one slot and has kept it over a number of draws, and published
   to the workers.
3. **The flat buffer from the draw state**, behind `BB_RENDER_BOUNDARY=flat`; `flat-check` also
   runs the walker and compares the two buffers (the check that mesh mode's "0 differing" was).
4. **Measured** against today's path in alternating runs: the workers' CPU time, the draws they
   prepare, frame time. Draws without a record or a frozen map keep the walker.

**What it gave** (Iosefka's Clinic, M3 Pro, the recompiled library):

- **Exact:** `flat-check` compared 51,738,285 sharps filled from the draw state with the walker's,
  0 differing, with every draw method's draws (N2.4). A plan keeps every candidate slot of a resource (slots that held the same sharp in
  every draw it was learnt from, as the engine's default samplers do) and is used only while the
  candidates still agree, so the right slot is always among them.
- **Coverage:** 88% of the shader stages of prepared draws: the vertex and pixel stage of every
  draw method's draw with a plan (140 programs), mesh draws (61%) and the methods' other draws
  (N2.4), at the same cost. Tessellated and geometry-shaded draws (their hardware vertex stage runs
  another shader), draws outside the methods (9%) and programs without a complete plan (3%) keep
  the walker.
- **Both threads:** the GPU thread checks a prepared draw by computing its flattened user data
  again, so it also fills the stages the worker filled, from the draw's record (`GpuFlat`), instead
  of walking: 3,725,047 of 3,725,047 such stages in a run. Without it the zeros the fill leaves
  between sharps failed the comparison and the GPU thread prepared those draws itself (92–129
  frames over 40 ms a run against about 35).
- **Cost:** with `flat` the game takes 378% CPU against 336% without the render boundary
  (alternating pairs, recompiled library), at the same frame rate. The fill itself saves no
  measurable CPU; the cost is the observer's. Four things keep it down: the functions its hooks
  enter stay recompiled (bloodborne-recomp's loader keeps a function whose only change is a hook at
  its entry), programs with a plan stop learning slots, records copy only each stage's used
  texture slots (a whole record is 7 KB), and plans hold each resource's candidate slots as a
  list. The rest (42 points) is the GPU command thread's time: 6.1 µs a draw against 4.3 without
  the boundary, so the GPU waits for work more often and the recorder submits about 10% more
  (430 a second against 392). It is neither the draw checks nor the record copies (each measured
  without a change): it is reading each record, written on a game thread, on another core. It must
  go before drawing from the engine can be the default.

N2.3's worth is the step N2.5 needs: mesh shaders' resources come from the draw state, not from
the PS4 user data.

### N2.4: the other draw kinds and compute passes

**Draws outside the seven methods** (render-target passes, Scaleform; 8.6% of draw stages): 81% of
their stages take every sharp from the user-data registers, so no walker runs and no boundary is
needed. With the methods' draws filled from the draw state (85%), 92.5% of draw shader stages run
without the walker; the rest are method draws whose program has no plan (5.9%) and draws
outside the methods that walk tables (1.6%). Those programs are few: of the programs learnt, 141
have a plan and 7 cannot (six read a buffer that matches no slot of the draw state, one a
sampler); learning for longer does not change it (plans after 16 draws instead of 256: one more).

**Compute passes.** 
Two kinds of dispatch reach libGnm (counted with `BB_RENDER_BOUNDARY=flat-check`, Iosefka's Clinic):

- **Through the resource binder (7%):** `CG::GraphicsContext::dispatch` and others call
  `CG::ResourceBinder::commitCompute` with a compute state, which holds one shader stage laid
  out as a draw stage and 8 writable resources (recomp's `annotations/structs.h`:
  `CgComputeState`). An entry hook there reads them into a record, libGnm's `dispatch` and
  `dispatchIndirect` file it and tag the packet, and the GPU thread, which prepares dispatches,
  learns each compute program's slots from the walker and then fills it from the record
  (`GpuComputeFlat`). Exact: 3,514 sharps compared, 0 differing. A tenth of these dispatch
  stages are filled; the rest belong to programs that also read a small engine-global buffer (3
  or 4 records, at the same address in every dispatch) through a table in memory, not from the
  compute state. Those keep the walker: filling it as a constant would go wrong unnoticed if the
  engine moved it.
- **The surface operations (93%):** `CG::SurfaceOps`' copies, fills and clears set their sharps
  in user data themselves and dispatch directly, with no compute state. 84% of their dispatch
  stages read every sharp from the user-data registers, which are already what the engine set:
  no walker runs and no boundary is needed. The other 16% walk resource tables.

So 79% of dispatch stages run without the walker; the surface operations that walk tables and the
buffers without a slot are what is left.

### N2.5: shaders ahead of time

**The decision (maintainer, 2026-10-30, 2026-10-31):** a launcher step, like the recompiled-code
build, converts the game's shaders once into the shader cache. It is a setting in the launcher's
recompiler settings that a player enables if they want it, because it takes time; nobody has to
wait through it. Converting at first use stays the default until the player runs it, and stays the
fallback for anything the step did not convert.

**What there is to convert** (the player's 1.09 dump):

- The shader bundles (`dvdroot_ps4/shader/*.shaderbnd.dcx`: DCX, DEFLATE, then BND4 archives)
  hold 15,300 files, one shader stage each (`.vpo`, `.ppo`, `.gpo`, `.dpo`, `.hpo`, `.cpo`). By
  the program hash the cache is keyed with (the `shader_hash` of the `OrbShdr` block Sony's
  toolchain appends to a shader's code), they are 3,382 programs: 1,730 vertex, 1,346 pixel, 167
  domain, 62 geometry, 39 hull and 38 compute.
- The executable holds 5,469 more programs of its own (the engine's built-in passes, Scaleform and
  middleware among them).
- A long play's cache (the maintainer's, several levels) held 533 programs in 845 variants: 423
  from the bundles, 110 from the executable. 364 of them (68%) only ever needed one variant.

**Variants are the hard part.** A converted shader is specialized for its draw: each vertex
attribute's number class, each buffer's stride and format, each image's view type and whether it
is an integer or writable image, the samplers, and settings such as instance step rates
(`Shader::StageSpecialization`). The shader's code fixes some of these (an image's dimension) but
not all. So the step converts each program with a predicted specialization, and a draw whose
specialization differs converts at first use, as now. **Where the first-play time goes** (Iosefka's Clinic from an emptied test cache, `BB_SHADER_LOG=1`,
M3 Pro): converting the game's shaders is cheap, 336 variants in 0.4 s (275 programs' first
variants, 1.2 ms each on average, 61 later variants). The cost is building Metal pipelines from
them: 589 pipelines in 2.9 s. 2.6 s of it falls in two frames as the level first appears (1.1 s
and 1.5 s); during play it is a few hitches of 50 to 230 ms when new draws appear. Later sessions
load the pipelines from the Metal binary archive.

**Those numbers had Metal's system cache warm.** Metal keeps its own compiled shaders in the user's
cache folder (`com.apple.metal`), across apps' caches and across a deleted shader cache of ours, so
emptying our cache is not a first play: libraries took a median of 0.6 ms. One run with that folder
moved aside too (`BB_SHADER_LOG=1`, the same place): 335 libraries in 8.4 s (a median of 14.3 ms,
the slowest 313 ms), 269 pipelines in 4.9 s (render 3.7 s, compute 1.2 s), the worst frame 4.9 s and
14 frames over 100 ms, 12.9 s in all. Library compiling, not pipeline building, is the larger part.

**What a step can reach.** A pipeline's key holds which vertex and pixel programs are paired, the
vertex formats, the render targets' formats, blending, write masks and sample counts. None of that
is in the shader bundles: the engine chooses it at draw time. So libraries can be built from the
game's files, while pipelines need keys that only a play session has recorded (`WarmUp` replays
those a player has seen).

**Compiling ahead of the GPU thread.** On Metal, `dynamic_vertex_input` is never set, so the
background pipeline path (`BB_ASYNC_PIPELINES`) does not run: every library and pipeline of a first
play is built one at a time on the GPU thread. Metal's compiler itself scales: 743 of the game's
shaders took 17.1 s on 1 thread, 4.6 s on 4 and 2.8 s on 8, and a source it has compiled takes no
time, in any process. The size optimization level saves nothing. So what a first play lacks is
lookahead, which the draw-preparation workers have:

- **Shader threads** (`PipelineFactory::Prefetch`, `BB_SHADER_PREFETCH=0` turns them off): a new
  module's library starts compiling at once on half the cores (2 to 6), and the pipeline build waits
  for what is unfinished. Alone it overlaps only a pipeline's own stages: stalls 12.9 s to 12.0 s.
- **Translating ahead** (`PipelineCache::TranslateAhead`, opt-in with `BB_SHADER_AHEAD=1`): a worker
  that meets a shader the GPU thread has not made yet has it translated on one helper thread, with
  the draw's own registers, so the SPIR-V is what the GPU thread will produce and `RegisterModule`
  gives it the same module. Nothing the GPU thread reads changes and no draw is skipped.

Two alternating pairs from empty caches (M3 Pro, Iosefka's Clinic):

| | Time in frames over 100 ms | Worst frame | Libraries |
|---|---:|---:|---:|
| Shader threads | 11.9–12.3 s | 4.3–4.4 s | 362 |
| With translating ahead | 7.0–7.2 s | 2.0–2.1 s | 385 (273 translations) |

The frame rate in play does not change (48.6–48.8 FPS). Translating ahead stays opt-in until a long
session has passed: the translator's asserts are fatal, and it now reads a pending draw's code ahead
of the GPU thread. The pipelines' 4.9 s is what is left of a first play's stalls.

**What tells variants apart:** of the 61 later variants, 37 differ only in their binding numbers,
20 in images or buffers (an image's type or sRGB, a buffer's format), 5 in runtime settings and
1 in vertex attributes. Of the 275 first variants, 172 use no integer vertex data, no image
other than a 2D one, no writable and no sRGB image.

### What N2 builds on

- N6's record of each draw: the five stages' shaders (code addresses), the draw state's
  textures, samplers, constant buffers and vertex buffers with their addresses, and the output
  state.
- N6's slot maps: per shader program which draw-state slot each resource it reads comes from, per
  vertex program and input layout which slot and offset each vertex attribute reads.
- Today's shader path: GCN code recompiled by the shader recompiler, its resources found by
  walking the user data, converted to SPIR-V and on the Metal backend to MSL; built pipelines are
  cached between sessions.

## The Metal backend today (N5)

On Apple Silicon `run.sh` selects it (`BB_METAL_FRAME=1`, `BB_GFX_BACKEND=metal`) unless
`BB_METAL_FRAME=0` asks for MoltenVK; Rosetta and other Vulkan drivers (`BB_VK_DRIVER`) use
MoltenVK. Linux keeps the Vulkan backend.

- **Start-up without Vulkan.** The native path links and maps neither the Vulkan loader nor
  MoltenVK. The shared renderer still uses Vulkan's types and calls some Vulkan entry points
  (instance, device, queue, capabilities, object creation); the Metal backend answers them
  (`mtl_vk_layer`, decision 6 of the [adapter](RENDER_ADAPTER.md#decisions)). Capabilities come
  from a recorded profile plus the MTLDevice (`mtl_vk_caps`). A C entry-point bridge
  (`mtl_vk_c_bridge`) lets optional Vulkan upscaler libraries resolve without the loader.
- **Resources.** Buffers, images, views and samplers are Metal objects; the game's memory is used
  in place (`newBufferWithBytesNoCopy`); everything the GPU may touch is in a Metal residency set.
  No VMA allocator.
- **Commands.** The adapter's Metal recorder (`mtl_recorder`) encodes draws, dispatches, draw
  state, bindings, barriers, clears, blits and fills; what the shared code still records as
  Vulkan commands (the scheduler's passes, transfers) is encoded by the Metal frame
  (`mtl_frame`). Submissions are committed on the backend's own queue; timeline semaphores are
  Metal shared events, fences Metal completion.
- **Shaders and pipelines.** SPIR-V to MSL with SPIRV-Cross (direct buffer, texture and sampler
  slots); rect lists rewritten as triangles; tessellation as compute kernels plus Metal's
  tessellator; geometry shaders as kernels. Built pipeline states are kept in a Metal binary
  archive between sessions (`PipelineFactory`).
- **Presentation.** The frame is drawn letterboxed into the window's `CAMetalLayer` drawable,
  with the settings overlay and FPS counter drawn by Dear ImGui's Metal backend.
- **Upscaler.** MetalFX only, encoded on the frame's command buffer with the backend's own
  textures; a setting asking for FSR or DLSS runs MetalFX. FSR 3.1 and FSR 4.1.1 are offered only
  with the MoltenVK fallback. Off by default.
- **Diagnostics.** `BB_METAL_TRACE=1` (command counts), `BB_METAL_CAPS=compare|moltenvk`.

The shader compiler, shader rewrites, pipeline cache and format and state tables originated in
[Huax-1839/bloodborne-macos](https://github.com/Huax-1839/bloodborne-macos/commit/0f0cf54) and
reached this project through
[Wooloomooloo2/bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port)
(`0b50589`, `78b59f5`), which further developed the Metal command layer and frame encoder. The
first Metal frame intercepted the renderer's Vulkan calls; the adapter replaced that route.
Crashes and their fixes are in [Stability](STABILITY.md).
