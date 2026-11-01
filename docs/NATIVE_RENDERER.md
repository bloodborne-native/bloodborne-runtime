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
| N2.5 · ahead of time | 85% `█████████████████░░░` | Designed and measured ([below](#n25-shaders-ahead-of-time)): a true first play, with Metal's own system cache empty too, spends 8.4 s compiling MSL into libraries and 4.9 s building pipelines; the worst frame takes 4.9 s. Shaders compiling on worker threads and translated ahead of the GPU thread (opt-in) bring the stalls from 12 s to 7 s and the worst frame to 2.0 s. Next: a long session with `BB_SHADER_AHEAD=1` before it becomes the default, then the pipelines' 4.9 s, which need keys the shaders do not hold. The optional step builds its list on the player's device from the game's files; translating from such a list reproduced 314 of 315 variants. The step builds shaders from the files and pipelines from states the player's sessions noted. Next: the shaders it still misses, and areas other than Iosefka's Clinic |

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

**What a step can predict from the files alone.** Measured by translating each of a cold run's 320
variants a second time with the draw's inputs guessed and comparing the SPIR-V (the same inputs
matched 320 of 320): with empty user data 114 matched, with the pixel inputs in plain order 291,
with one colour format for every target 199, and with all three 25 (8%). The guesses are simple
ones and a bad guess can stop the translator, which treats it as fatal: a guessed colour format's
export stopped one run. So guessing does not work: the step has to work the draws' state out from
the game's files (below).

**Compiling from a list works.** 314 of 315 variants of a cold run came out identical when
translated with no user data and no game memory, from descriptors holding only what a variant's
key records (no addresses or sizes, no faults; those runs kept the draw's runtime info and parsed
vertex attributes). What a step must supply is each program's key, not memory.

**What the files give a list.** All 260 programs of that run are in the game's files, by hash: 118
of the 320 variants in the model shader bundle, 95 in the other bundles, 107 in the executable.
15,033 models read without a failure in 58 s use 276 pairs of material, vertex layout and
skinning, and 2,000 sampled models 30 layouts. Materials name 81 of the 97 model programs used,
with substitutions by name; the whole model bundle, 14,571 files, reaches all 97, so a step can
compile every program without choosing among them. Each shader file carries its compiler's
reflection (resources, inputs, outputs). Among the 44 model pixel variants the pass decides the
colour formats of 43, and every sampler is the default one. The reflection's texture records give
the cube images of all 44 pixel variants.

**Which descriptor fields a variant's SPIR-V depends on** (one cold run, each field flattened
alone, 313 of 315 matching before any was): an image's sRGB and data format change nothing. Plain
swizzles change 13 variants, one 32-bit float format for every vertex attribute 76, and a stride
of zero for every buffer 135. So a list needs buffer strides and vertex attribute formats, some
swizzles, and not the textures' formats. With every buffer's stride 16 outside compute, four
elements per attribute and each attribute's format taken from its number class and swizzle alone,
314 of 315 still match, as many as the full key.

**What remains to predict.** A vertex shader's input records give the attribute count of all 74
model variants, but on held-out programs the records alone give the class and swizzle of 154 of
178 attributes and 19 of 34 whole programs: the rest needs the models' vertex layouts. Real
attributes use 10 formats. Pixel inputs are in plain order in 119 of 147 variants and shifted by
one in 14; pairing vertex and pixel shaders by name found a partner for only 25 of 103. The 147
pixel variants use 15 colour sets, 12 of which cover 144. The executable's programs have no
reflection; 41 of its 43 pixel variants read their inputs in plain order and they use 4 colour
sets. One variant per program can reach at most 260 of the 320 variants (231 programs had one);
a list needs a few candidates for the rest.

**Binding starts.** Pixel shaders are made first, so a vertex shader's bindings start after its
pixel shader's. Binding numbers are renumbered for Metal anyway, but the buffer start was also an
index into the shaders' buffer offset table, which made a vertex shader's code depend on its
pixel shader. The Metal renderer now binds storage buffers at exact offsets and has no such table
(storage alignment 4 instead of MoltenVK's 16). With the start at zero and binding numbers and
debug names ignored, all 315 variants then match, the rules above included. An empty-cache pair:
46.4 FPS against 43.8, stalls 11.5 s against 13.0 s, the same picture (one pair, so no gain is
claimed). Each stage's bindings now start from a base of its own (`Bindings::EnterStage`, Metal
only), so the start in a variant's key is the same for every partner: a first play in Iosefka's
Clinic makes 287 variants instead of 320 and 326 libraries instead of 362.

**With no descriptor table at all.** Every buffer and image a default descriptor (stride 16, a
plain 2D image), vertex attributes by class and swizzle: 246 of 248 graphics variants of a cold
run match, given each draw's runtime info and which images are cubes (without the cubes, 21 fewer
over all stages). Compute matches 4 of 34: every compute variant uses a storage image or a
formatted buffer, whose real formats a default cannot give, so compute stays at first use for now.
Tessellation and geometry stages also stay at first use: they read constants a default cannot
give.

**What a shader's own file gives.** All 8,851 programs, in the bundles and in the executable, have
a `Shdr` header with the stage's registers. From a list built from those headers alone, 276 of
282 variants of a cold run match (pixel 144 of 147, vertex 99 of 99; compute kept its real table,
and attributes and colours are still real). What the header holds:

- User data and register counts, and the enabled pixel inputs: all programs checked.
- Pixel input order, with no vertex partner needed: each pixel shader's table of input semantics,
  minus the first export semantic, gives all 104 programs with files (the tables were not checked
  on the executable's programs).
- Colour export formats, one nibble per target: 138 of 147 pixel variants. The other 9 declared an
  output in a draw with no colour target; on Metal a pixel shader now always exports what it
  declares, and a target that is not bound ignores it, so there is one form.
- Vertex inputs (semantic, register, declared size): the table is there in all 130 programs. The
  declared size is not the mesh's: components taken from it alone keep 56 of 99 vertex variants,
  and classes alone cost far less (263 of 282 overall, against 227 with both from the list).
  Components from the semantic and declared size together are right for every attribute in 82 of
  96 programs with reflection (fitted on the same programs, not held out); 20 of the executable's
  34 fit with no names, and most of the rest declare 4 components where the mesh supplies 2.

Images that are cubes come from the code: V_CUBEID's result reaches the sample's coordinates, right
in 280 of 282 variants. A colour target needs only its number class: on Metal a pixel shader exports
four channels and each draw says where each component goes (`PushData::mrt_order`), so the channel
order is no longer part of a shader. One alternating pair showed no FPS cost and the same picture,
and none of a first play's extra variants comes from colour. Vertex keys no longer hold step rates
a shader cannot read or the tessellation flag when it has no output map. A first play in Iosefka's
Clinic now has 260 programs and 21 extra variants: 15 compute formats, 3 pixel images, 3 vertex.
Clip-disable stays a real difference, because a rect list converts the position in the shader.
Still to supply: vertex attributes' components and classes where the rules above miss.

**A list from the files, measured.** A local prototype translated programs from the game's files
before play, with up to four candidates each, and looked every real variant up by its SPIR-V: 186
of the 249 graphics variants of a cold run were already there (pixel 120 of 147, vertex 66 of 102),
186 of 287 with compute and tessellation, which stay at first use. All 8,373 vertex and pixel
programs but one took 18.2 s on one thread: 31,684 translations, 23,470 distinct shaders, no faults;
501 translations (167 vertex programs) reach an assert that must be survivable, and one program
ended the run until skipped, cause unknown, so the step has to isolate failures. At the measured
compiler speed those shaders would take very roughly 90 s; that is not measured. With only that
session's 614 shaders compiled before play, a true first play's stalls fell from 10.9 s to 7.5 s
and the second-worst frame from 1.6 s to 0.85 s (one run), but the worst frame stayed at 3.2 s:
what remains is mostly pipelines, which a list of shaders does not build. The step is not built.

**What decides a pipeline.** The game compiles nothing while it runs: its shaders are finished
binaries and each draw names a shader pair and its state, so only our conversion has a first use.
212 real graphics pipelines of a cold run use 39 distinct state sets. The engine builds a
material's shader set when it loads the material, from a fixed list of 21 passes: the pixel shader
is the material's shader plus the pass name, with alpha variants, and the vertex shaders add
skinning, morph, instancing and clip options, most passes using their own and a few another
pass's. That rule gives all 85 model pipelines' pairs (matched by program hash: identical binaries
have up to 156 file names), and 7,959 distinct model pairs in the whole game. In those 85, each
vertex program has one vertex format set, all draw triangle lists, and each pass has one state set
except `gbuf` (3) and `fwd` (2). The other 127 pipelines, the fixed passes, are not predicted yet;
the engine names many of their shaders directly, and 64 use the executable's own programs.

Timed in one cold run, model pipelines take 3.6 s and fixed-pass pipelines 5.1 s of the 8.7 s spent
making them, shader waits included, and the slowest moments hold both, so a list of model
pipelines alone does not remove the worst freeze. Pairing fixed passes by what a vertex shader
exports and a pixel shader reads keeps every real pair (113 of 113) but allows 607,418: too many.
The executable's 5,498 programs are three groups: 5,139 in a post-effects library of permutations
(45 used in that run), 338 for the interface (31 used) and 16 mostly compute (23 variants used).

Every real pair stays inside one source: one shader bundle, the post-effects library or the
interface library. Of the library's programs, 2,644 name an HDR encoding that run never used and
are left out. Pairing a pixel shader with the vertex shaders of its own source that export exactly
what it reads gives 14,030 fixed-pass pairs: 8,464 post effects, 4,127 interface, 1,436 in the
bundles, 3 others. With the 7,959 model pairs that is 21,989, which hold 187 of the 212 real
pipelines (85 model, 102 fixed). 11 more read fewer outputs than their vertex shader exports, and
allowing that takes the fixed pairs to 77,137, so they stay at first use; so do 9 depth-only
pipelines and 5 with geometry or tessellation stages. The exact pairs are an upper bound, not the
engine's rules, and no pipeline has been built from them yet.

**What a step must fill: Metal's cache.** A first play with our cache empty and Metal's system cache
warm stalls for 1.7 s with a worst frame of 0.73 s, against 10.9 s and 3.7 s truly cold, and 0.6 s
and 0.23 s with both warm. So most of a first play's stall time is Metal compiling, and running each shader and pipeline
through Metal once removes it. That is not enough to play without hitches: on a first play the game
still translates, converts and creates every object it needs, and whatever is new hitches (see
"What a first play still does"). The step has to write our cache as well, as decided on 10-30.

A prototype did that for one session, truly cold: it translated the session's programs from the
game's files with up to five candidates, then built the session's pipelines through Metal with the
state recorded from an earlier run, before play. First-play stalls fell to 3.5 s and the worst
frame to 0.74 s. 226 of the 249 graphics variants were there (pixel 142 of 147, vertex 84 of 102);
compute (34) and tessellation (4) compiled in play. The state was recorded, not predicted, and the
list held only that session's programs, so this is the best case. The header also gives the
front-face flag of 24 pixel variants, and an integer attribute's missing channels read 0 where a
float's last reads 1.

At full size the list held: all 8,373 vertex and pixel programs but one took 25,013 shaders, compiled
through Metal in under 5.5 minutes, and Metal's cache ended at 273 MB. A first play after that alone
(the game exited before drawing anything) stalled for 7.9 s with a worst frame of 2.0 s: the
pipelines' 3.9 s remained.

**Pipeline state predicted.** A shader knows what it outputs, not where it goes: header fields give
all of a pipeline's state for 148 of 203 pipelines, fitted on those same pipelines. With the render
pass added and each pipeline predicted only from the others, the real state is the first guess for
139 of 206 and among the first three for 167. A cold first play with that session's shaders from
the files and three predicted states each built before play stalled for 5.5 s with a worst frame
of 0.82 s. The prediction still took its states from other pipelines of the same session, kept each
pipeline's real pair and vertex formats, and took 4,098 Metal builds for 206 pipelines. That run
made 342 variants, not 287, for a reason not found.

Each colour state implies its pixel shader candidate, which halved those builds to 2,282. Compute
headers hold every register: workgroup size, user data, register count (a plain multiple of 4) and
the thread ID flags. Only formats are left, so each of the game's 68 compute programs is tried with
16 pairs of format and channel order against 5 image types: a storage image has the format and
order of the buffer beside it. That gave 25 of 35 compute variants; programs mixing several buffer
formats and large strides stay at first use. With shaders, compute and predicted pipelines for one
session, a cold first play stalled for 4.9 s with a worst frame of 0.77 s (one run).

**The whole game, generated.** A vertex attribute's real format comes from the mesh, not the
shader: Metal compiles it into the pipeline. For models the semantic decides it, with `TEXCOORD`
following its size; other sources follow a few patterns tried as candidates. The executable's
programs, which have no pass name, are grouped by the family in their own name. Pairs and states
generated this way hold 186 of the 206 real pipelines of a cold run (pairs 193, states 193 among
two, formats 204); the state table still comes from that one session, so other areas are untested.
Metal built every shader and 97,722 pipeline candidates, with the game drawing nothing: the
pipelines took 174 s on one thread and Metal's cache ended at 392 MB. 66,744 generated pipelines
leave 5,450 pairs with no known state. A first play on that cache stalled for 4.9 s with a worst
frame of 0.86 s, Metal's pipeline work 1.1 s against 3.7 s (one run). One run on that cache stopped
on a stale descriptor (data format 15), unrelated to this work; Wooloomooloo2's safe read
(`UPSTREAM.md`) addresses that class. A progress bar over the build drew correctly in the prototype;
its shader total grew as shaders were found, so a real step needs the total first.

**What a first play still does.** Charged R2 in the plain game made 34 new variants, 9 frames over
100 ms and a worst of 425 ms; on the same cache a second time 2 variants and none over 100 ms. Of
309 variants a first play made with the whole list translated beforehand, 270 were already there:
the game had no way to find them. A lookup by what the output depends on, read without translating
(image kinds, formatted buffers, vertex attributes, output maps, pixel inputs and export formats),
found 323 of 405 with none wrong; the vertex output map has to be part of it, since clip distances
come from a draw register and only appear with some draws. The other 39 of the 309: vertex 19,
compute 10, tessellation 6, pixel 4. Translating is about 1 ms a shader; converting to Metal and
creating libraries and pipelines is the cost. Keeping the compiled store from the compile run took
library time from 2.2 s to 0.8 s. Loading a Metal binary archive of all the pipelines stops the
game for over a minute on one thread before anything is drawn, so that archive cannot simply hold
everything. Without an archive, pipelines are made in play (42.6 FPS in that run).

**With a shader independent of its draw.** A pixel shader's colour outputs and a vertex shader's
attributes no longer depend on the target or the mesh: Metal fills a vertex attribute's missing
components, so a V# that says the same needs no swizzle, and an integer attribute takes its fourth
component from the draw (`PushData::attrib_w_zero`). Output maps are safe to give every vertex
shader in the step: a shader without those exports does not change. Against shaders generated from
the game's files, a first play matches pixel 138 of 142, vertex 92 of 101 and compute 24 of 34;
tessellation stages are not generated yet. The rest compile at first use. Compute's misses are a
few generic copy programs over a fixed set of formats plus mixed-format programs, so they keep
several forms rather than decode formats in the shader.

The local prototype, whole game (12,553 shaders, 66,454 pipelines in 104 s on one thread, Metal's
cache 268 MB), then a first play with empty test caches apart from Metal's and the compiled store,
one run of each: the scripted run had 2.7 s of stalls, all while loading (723 ms worst), no compile
in any window of play, a worst frame in play of 67 ms and 49 FPS. Charged R2 eight times: 16
compiles and 120 ms in all, a worst frame of 76 ms. Only Iosefka's Clinic has been measured.

The step is in the runtime (`BB_SHADER_COMPILE=1`, `BB_GAME_DIR` the game's folder): `vk_shader_scan`
reads every program from the game's files on the player's device, the compile translates them,
shows compiled of total on an overlay, and records each finished shader in the cache's
`shaders-ahead.txt`. A stopped run continues from that record (150 s instead of 210 s after
stopping at 3,145) and a finished one is not repeated until the shader version changes. 8,441
programs give 12,554 shaders in 3.5 minutes on an M3 Pro and 207 MB of Metal's cache. Play needs no
lookup: Metal finds a shader by its content. With this step alone a first play has no compile in
any window of play; pipelines are made at first use, most while loading (1.9 s worst frame, one
run). The stopped run's record held 97 hashes the next run did not make, so some translations
differ between runs; the cause is open. 167 vertex programs stop the translator and are skipped.

**Pipelines in the step.** The step needs no recorded play. The planner (`vk_pipeline_ahead`) holds
the engine's rules, and works on the shared pipeline key, so any backend can use the same plan; only
building and caching is the backend's. A model pixel shader's file name gives its pass and the pass
gives its targets: `dpt` writes depth only, `dptv` one two-channel target, `velo` one, `pnt` two,
`gbuf` six or seven, and the forward passes one target with the material's blending. The executable
holds the YEBIS effects and, after a wide gap in the file, the engine's own programs: each has a
small family of states, as do the effect bundle and the other full-screen passes. Vertex formats
come from input semantics; without a semantic, a shader's read size only hints at the format, so
likely formats are crossed for short layouts. Pairs of shaders come from the game's files (model
shaders by file name and pass, the rest by a vertex shader that exports what a pixel shader reads).
Each pipeline is built into Metal's own cache and released: it is neither kept in memory nor put in
the binary archive. Nothing is shipped.

With the setting on, each pipeline a session builds is still noted in the cache's
`pipelines-seen.bin` and added to the next plan; the step returns when four new states have been
seen, so that what the rules miss is still built. `BB_PIPELINE_PLAN_CHECK=<file of noted pipelines>`
(with `BB_SHADER_COMPILE=1`) prints how many of those the rules alone plan, then exits: 295 of the
308 of one Clinic session. Unused colour slots hold stale registers that no backend reads, and the
check ignores them.

A compute shader's form depends on what each of its bindings holds, and a game's copies, clears and
effects mix them. Two things keep that small. An indexed compute buffer reads its stride from the
sharp in the flat data when the shader runs, so the game's structure sizes are not forms at all; the
copy and clear replacements do not count the flat buffer those shaders then bind. And the step tries
the mixes of a program's formatted buffers and images (bounded at 2,000 per program, with shorter
lists past that); sources that come out the same are one. In one Clinic session 33 of the 34 compute
forms played were built ahead, up from 26. Compute shaders are the only stage done this way.

Two translations of a shader can differ only in SPIRV-Cross' numbered names. On Metal, the text is
renumbered in order of first use and the function is named after it, so such translations are one
text, one library and one entry in Metal's cache (`CanonicalizeMsl`; a text must stay in one library,
for the binary archive). After the step, 253 of the 295 sources one Clinic session compiled were
already built: 132 of 136 fragment and 83 of 93 vertex sources; the rest are the runtime's own
kernels, tessellation stages, and vertex and fragment forms that still differ.

The render state itself lives in engine objects (`CgBlendState`, `CgDepthStencilState`,
`CgRasterizerState`, in bloodborne-recomp's `annotations/structs.h`). The engine creates its
targets and states early: by the 2,000th draw 175 of the final 177 colour targets and all 89 depth
targets exist, in eight colour formats. Blend states are a generated list of about 57 entries;
rasterizers are 192 entries indexed by an 8-bit key from the object at image offset 0x5540dd8. The
planner does not read these at run time.

From empty caches with nothing recorded (one run, Iosefka's Clinic, standing still):

| | |
|---|---:|
| Whole step | 413 s (shaders 405 s, of which pipelines 192 s) |
| Pipelines planned | 3,326,185 |
| Built / failed / no shader | 2,598,447 / 656,734 / 71,004 |
| Metal's cache | 453 MB |
| Worst frame, first play | 786 ms |
| Frames over 100 ms | 11 (3.9 s in all) |
| Last frame over 50 ms | 21.7 s of 74 s |

Why pipelines fail to build is not broken down. A session with recorded states in the same place
is the better case: worst frame 258 ms, 7 frames over 100 ms, against 1.9 s with no step. The stalls
above fall in the load into the level. Comparing the Metal source compiled in the step and in play:
the 277 sources the step built cost 119 ms in all in play (6.5 ms at most), and the rest of the
compile time, 1.9 s, is 63 sources it never built: 30 forms of the game's shaders (compute most of
all: buffers of differing types and strides, which one format per shader does not reach; also 3D
images), 16 tessellation variants, 15 of the port's own compute kernels and 2 rect-list vertex
shaders. Moving, fighting, menus and other areas are not measured. Step attributes are built per
vertex, so instanced pipelines may still miss Metal's cache.

**Progress for the player.** Cache entries are named by program hash and permutation, so the step
can list what is compiled, skip it, and show compiled of total; what play compiled counts.

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
