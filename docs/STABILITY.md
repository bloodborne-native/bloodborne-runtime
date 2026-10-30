# Stability

[← Documentation](README.md)

The crashes and corruption found so far, their causes and fixes, and what is still open. The
"Release" column is the app release ([bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac/releases))
that first shipped the fix.

## Fixed

| Problem | Cause | Fix (switch to turn it off) | Release |
|---|---|---|---|
| GPU assertion while loading, `Unimplemented PM4 type 0` (most loads in Brazilian Portuguese) | The game's main thread and its render thread wrote one command buffer at once | A lock per command buffer around the game's command-buffer methods (`BB_CB_GUARD=0`); command buffers copied at submit | 0.5.1 |
| Opening video: a null read (address `0xd2`) before character creation, 2 in 5 openings | The render thread cleared the shared graphics input state between the movie's bind and draw | The movie frame and the render task lists hold the same buffer lock across their state changes and draws (`BB_MOVIE_STATE_GUARD=0`). 10 of 10 guarded openings completed, against 3 faults in 5 controls; about 0.5% more CPU | 0.6.0 |
| Opening video: audio thread panic `Invalid mutex` at shutdown | The AV player's queue read its size and front without its lock | Every queue access locked; tested under ThreadSanitizer. The playback clock's unsynchronized timestamp pair also fixed | 0.6.0 |
| Stretched scenery ("vertex explosions"), missing walls, Yharnam's black sky with streaks and objects popping in, after visiting several areas | MoltenVK draws wrongly from data past 4 GiB in one Vulkan buffer; the game's pool is 6 GB | The pool imported in windows so no binding offset reaches 4 GiB (`BB_POOL_SPLIT=0`) | 0.7.0 |
| GPU device lost (`VK_ERROR_OUT_OF_DEVICE_MEMORY`, "Invalid Resource") with character motion vectors, often in boss fights | GPU objects freed while command buffers still used them: residency-set removals, texture-cache deletions before the draw pipeline recorded its prepared draws, large staging buffers MoltenVK's argument buffers still referenced | Removals wait for the submissions that could use them; deletions one submission later; large staging buffers kept (`BB_STAGING_TRIM_LARGE=1`: trimmed) | 0.8.0 |
| 1440p and 4K stopped at the loading screen on an 18 GB Mac (device lost) | 9 GB of direct memory for every output above 1080p, imported twice (17 GB resident) | Memory sized from the scene; the pool imported once, in windows | 0.8.0 |
| A crash in MoltenVK (`isTessellationPipeline`) when a draw used a pipeline the driver could not create | The failed pipeline was skipped only the first time; later lookups returned it empty | Every lookup skips it | 0.8.0 |
| In-game restart froze | A thread blocked in `fgets` held stdin's lock and the restart's `fflush` waited for it | The launcher channel reads with `read(2)` | 0.8.0 |
| Stale texture formats, garbage PM4 headers, null vertex-buffer tables, flip interrupts ahead of their label stopped the game | Rare bad states | Each is logged and skipped (from the Supermedo fork) | 0.8.0 |
| Blur-kernel assertion `0xa0020007`; crash at `0x2199310` (GX command heap full) | Game data out of range; a 256 KiB per-frame heap | Logged and ignored; 1 MiB heaps (`BB_GX_HEAP=0`) (from bbport 0.6) | 0.8.0 |
| Metal recorder fault in `BindStage` while loading | Metal pipelines held shader pointers after Vulkan reused a module handle; queued draws held pipeline entries after deletion | Shader modules kept by SPIR-V content, bound pipeline entries shared through deferred draws | 0.8.1 |
| GX worker null access during a copied-save load, on Metal and MoltenVK | A live setting turned dynamic shadows back on in a secondary render context without a shadow target | Keep the game's disabled shadow state for that context while retaining the live setting elsewhere | 0.8.1 |
| Shader cache rebuilt at every launch after a rejected tessellation pipeline | The generated shader assigned one output location twice, and the rejected pipeline key was still saved; preload treated the driver rejection as a damaged cache entry | Emit each location once, rebuild old shader binaries once, and save only successful pipeline keys | 0.8.1 |
| Character clothing lost its lighting above about 3.5 million scene pixels on Metal and MoltenVK | The larger scene crossed the texture collector's cleanup mark sooner. It evicted small GPU-written images marked possibly CPU dirty without preserving their GPU contents | Keep GPU-written images whose contents cannot safely be downloaded until a definite CPU write supersedes them; normal collection of other images continues. They are kept at every memory level, the critical mark too: accuracy before memory (a lower frame rate rather than dark lighting). Images a later GPU write superseded (GpuDirty) are not kept: their data is in the buffer cache (from maze1377's SpartanPort port, PR #1124) | 0.8.2 |
| Metal API validation: the presented frame's view destroyed while a Metal command buffer still needed it, below native render size (225–463 reports in 75 s); no visible fault | The presenter made a view of the upscaler's display image every frame and freed it after the draw submission, but the Metal path presents later | The view is kept with the display image and freed with it, after the GPU is idle: 0 reports (`41f289f`) | 0.9.0 RC 1 |
| Metal API validation: the camera-motion debug overlay's (`BB_DEBUG_MOTION=1`) image views destroyed while still required (78 reports); found before a release | The overlay made two views a frame and freed them after its dispatch, which the recorder can place in a later command buffer | The overlay uses the texture cache's views: 0 reports (`0f27507`) | 0.9.0 RC 1 |
| Metal memory grew with every image the texture cache freed (at the test save: 526 live textures, 795 MiB, against 336, 647 MiB); introduced after 0.8.3 | `~UniqueImage` destroyed only the image's Vulkan handle, not the Metal texture of an image made in Metal | The destructor releases the texture too; the periodic `Memory: Metal` line prints the Metal device's allocation and the live textures (`a0c587f`) | 0.9.0 RC 1 |
| The item atlas drawn over gameplay and a dark coat with an explicit `BB_RENDER_RES=1920x1080` | An explicit native size took the scaled-session UI path | An explicit native size is treated as native and its resolution patch is skipped (`c7604ce`) | 0.9.0 RC 1 |
| Metal aborted opening the status page in the Lecture Building, and on quitting (`newTextureViewWithPixelFormat`) | The Metal backend accepted block-texel-view images, so the texture cache viewed block-compressed images with uncompressed formats, which Metal cannot do | The Metal backend refuses such views; the texture cache keeps a compressed image and an uncompressed one apart on devices without them (`TextureCache::CanView`). Checked on an M1 Max (`82051e5`) | 0.9.0 RC 2 |
| Walls and scenery black on every start after a long session, until the shader cache was deleted ([app issue #7](https://github.com/bloodborne-native/bloodborne-mac/issues/7)) | Cached shaders are named by program and permutation index, and a new permutation took the index of a cached one a session had not loaded: it overwrote that shader, and every pipeline key naming it loaded the new one (82 of 697 keys in the reporter's cache) | A new permutation takes the first index past every cached one; motion vertex shaders are no longer cached; cache version 22 rebuilds existing caches once (`8bc0ce6`) | 0.9.1 |

## Open

- **Crash on death in Central Yharnam on Metal**
  ([app issue #5](https://github.com/bloodborne-native/bloodborne-mac/issues/5); 0.8.3, M5
  MacBook Pro, FSR 3.1, three mods). Metal aborted when a depth-stencil image
  (`Depth32Float_Stencil8`) was viewed as a colour format (`RGBA16Float`) while the game loaded.
  Closed without the reporter's answers; the cause is unknown. It is a different case from the
  block-compressed views fixed in 0.9.0 RC 2. Reopen with a new log.
- **GXRenderThread null read at guest `0x2169ed9`: guarded on main** (bbport 0.7's fix, whp199's
  PR #343: an occlusion query whose creation failed is created again or not begun; the next
  release). Kept open until a long session passes without it.
  Before:
  ([app issue #3](https://github.com/bloodborne-native/bloodborne-mac/issues/3), 0.8.0, MoltenVK,
  M1 Max, Cathedral Ward; [#7](https://github.com/bloodborne-native/bloodborne-mac/issues/7),
  0.9.0 RC 2, Metal, M3 Pro, Central Yharnam's sewers). The command-buffer writer receives a null
  resource from a render-task entry the game filled; the guard on that buffer was held. Once in
  each report, after one to four hours of play; not reproduced here. Its signature differs from
  the GX worker fault fixed in 0.8.1.
- **The Rosetta (x86_64) build faults before its first frame.** It reaches device initialization,
  then faults before a frame; an isolated build of main before the Metal backend's last step
  (M4 step 4.4) faults at the same guest offset and thread, so that step did not cause it. The
  cause is unknown.
- **Pale blood after a save reload.** Blood on enemies already killed when the save is reloaded
  can look pale with bright edges instead of dark red. The cause has not been isolated.
- **Missing objects or character parts after death reloads** with the translator, not under
  Rosetta 2: a candidate bbcpu translation bug. Not seen since the pool split (0.7.0), not
  retested separately.
- **Two Metal API validation reports** present since the Metal backend's step M2: pipelines with
  a depth or stencil format used in passes without that attachment, and one compute shader
  reading 32 bytes from a 28-byte buffer. No visible fault is linked to them.

### Watch list

Seen during development and not reproduced since; count each new occurrence with its build and
run.

- A null guest virtual call during a native 1080p Metal test, once.
- A short white flash in the opening video, in older runs.
- One enemy's body drawn as colour noise after it attacked the player, once.
- `vk_shader_hle.cpp ExecuteCopyShaderHLE: Assertion Failed!`: 3 times (once loading, twice in
  the opening video).
- `liverpool_to_vk.cpp MipFilter: Unreachable code!`: 5 times, all in the opening video. Caught at
  draw enqueue with valid table indices: the invalid sampler comes from the table's contents, not
  from later corruption of the queued draw (`BB_SAMPLER_PROBE=1` names where it came from).

## Ruled out for the stretched geometry

Before the pool split fixed it: FaceGen, every switchable bbport optimization, the draw pipeline,
copy hoisting, submission locking, texture-collector pressure, shader bounds checks, vertex fetch,
index and restart handling, timing and cached fetch shaders. The data in memory was intact; only
draws reading past 4 GiB in one buffer were wrong.

## How to investigate

- Reproduce with a copied save (`tools/test_run.sh`, `SAVE_FROM=` a backup near the problem) or
  the opening video (`tools/test_opening_video.sh`), alternating a control with one change.
- A loading assertion or `Bad PM4 packet`: run `BB_CB_PROBE=1` first.
- A GPU device loss: Metal API validation (`MTL_DEBUG_LAYER=1
  MTL_DEBUG_LAYER_ERROR_MODE=nslog`); the window's BGRA8 sRGB drawable warning is noise.
- A fault in translated code: `BB_JIT_FAULT_MAP=1` names the exact guest instruction;
  `BB_JIT_WATCH` records selected loads and stores.
- Thread and lock problems: `BB_PTHREAD_PROBE=1` logs failing lock operations and the last mutex
  destructions; `BB_AVPLAYER_TIMING_PROBE=1` the video's timing; `BB_SAMPLER_PROBE=1` where an
  invalid sampler came from.
- Bisect bbport's optimizations with `BB_TOGGLE_FILE` (bits 0–19, 30, 32–50).
- A hang in an indirect dispatch: try `BB_INDIRECT_GUARD=0`.

Diagnostic output that contains game memory or addresses stays in the ignored `out/` folder,
never in Git or a wiki.
