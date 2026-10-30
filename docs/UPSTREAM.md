# Upstream: bbport and other forks

[← Documentation](README.md)

This runtime started as a fork of [bbport](https://github.com/deadinside28/bloodborne_pc) by
deadinside28, the native Linux port of Bloodborne, and keeps taking its work. bbport's own
documentation (release notes, design notes, mostly in Russian, about the Linux version) is in
its repository; this page says what we took from each release and from other forks.

## bbport releases

| Release | How | What |
|---|---|---|
| Up to 0.3; 0.4 (`524f829`), 0.5 (`5c397fb`) | The fork's starting point; then merged with history | The loader, the PS4 runtime, the renderer's extensions (parallel draw preparation, the two-stage draw pipeline, copy hoisting, memos), temporal upscaling (FSR 3.1, FSR 4), motion vectors, the community patches, mods |
| 0.51 (`3c57da4`) | Merged with history (`242575c`) | Buffer and texture cache fixes, mouse look, the online module (`gpu/bbnet`, built on Linux; macOS builds `gpu/bbnet20`, below) |
| 0.6-pre1 (`72e5c8c`), 0.6 (`8185b88`) | Ported by hand: 0.6 replaced shadPS4's renderer with bbport's own, and its hook and menu code calls that renderer, so a merge does not fit | See below |
| 0.7 (`0fc2921`) | Ported by hand, item by item | See below |

The compatibility level imported from bbport is **0.51**, plus the 0.6 items below.

### From 0.6

| Item | State |
|---|---|
| Push constants written after the render pass begins (single frames shifted by half the screen); draws without a pixel shader rebuild their own fragment info; cached geometry-stage pipelines keep their fetch shader | Ported |
| The present waits for its frame at every stage (single black or grey frames) | Ported |
| GPU-written images keep their mip hashes from the first upload (an empty character-creator preview, #178) | Ported |
| Decoder and format asserts during draw preparation (#144, #165, #166) | Already here (stale formats logged and skipped) |
| Draw preparation on a quarter of the hardware threads on 12+ thread CPUs | Not needed: ours uses half |
| Blur-kernel assertion `0xa0020007` ignored; GX per-frame command heaps of 1 MiB | Ported (the heap as a patch) |
| Frame rate through hooks, live limit, F3 for 30 FPS | Ported (`bbport_frame_rate.cpp`) |
| Game-code hooks (`InstallGuestCodeHook`) | Ported as an interface; our implementation runs its stubs in bbcpu |
| Laurence's cinematic above 30 FPS (job slots, 512 MiB render cache) | Ported (`bbport_game_hooks.cpp`) |
| Effects switched live (with bloom, a new setting), SSAO only where the game has its target, SSR at start | Ported |
| Aspect ratios through hooks (camera, HUD, markers, UI viewport), the UI edge stretch and its 21:9 streak fix, the blur-kernel clamp | Ported (16:9 stays the default: Auto would change a player's output by itself) |
| Keyboard and mouse: the Dark Souls III layout with chords, the movement-key ramp, short taps, native mouse look, the menu pointer, the port's cursor, the mouse-capture fixes (`runtime_pad.c`, `window.cpp` merged from 0.6) | Ported |
| Button prompts for the device in use: the game's key-guide icons and HUD atlases known by their bytes and sampled from the port's icons for the keyboard, mouse or a non-PlayStation pad (`bbport_button_prompts.cpp`; drawn through the render adapter, `vk_prompt_images.cpp`, on Metal and MoltenVK); chords with Shift, Ctrl or Alt in the input capture (`bb-gpu-capabilities --read-input`) | Ported |
| Online on shadPS4 0.20's network code (Seamless Hunt with BBSC co-op, The Hunter's Dream) | Done on macOS without BBSC (`49e2d2c`, `gpu/bbnet20`): taken from shadPS4 0.20 and droogie/bbhost directly. Seamless co-op is left out (it is built on the game's internals) |
| FSR 4.1.1 changes | Not taken: on macOS FSR 4.1.1 runs in Metal (fsr4-native-macos) with the MoltenVK renderer only; the Metal renderer offers MetalFX only |
| The new memory model, NVIDIA/Intel/VRAM fixes, Steam Deck, AppImage | Not applicable (unified memory, Apple GPUs, macOS) |
| FSR 4.1.2 on macOS | Ported from fsr4-native-macos v2.2: FSR 4.1.2 Lite, 4.1 Lossy and 4.1 in Metal (`gpu/fsr4_metal/`, VENDOR.txt) |

### From 0.7

| Item | State |
|---|---|
| The fault at `0x2169edc` on GXRenderThread: GX occlusion queries without a handle (bbport #336, PR #343 by whp199; app issues #3 and #7) | Ported: the same guard as hooks at the begin's and end's entries (`InstallGxQueryGuard` in `bbport_guest_hooks.cpp`), installed from `bbgpu_patch_image` on every platform, since bbport's x86 stubs need a stub page macOS does not have. `BB_GX_QUERY_FAIL=2` ran a scene with every second creation failing, without a fault; with the guard on and off, 38.6–39.4 against 34.1–38.7 FPS (no cost) |
| Cloth fluttering above 30 FPS (#58): Havok's stiffness factor per substep scaled by the frame's length (`ClothStiffnessFactor` in `bbport_frame_rate.cpp`), the cloth setups' bound back to the game's 1.0 | Ported: in Iosefka's Clinic at 39–40 FPS the factor is ×0.75–0.78 (×0.5 at 60 FPS), no faults with the translator or the recompiled library |
| Engine fixes from SrShadowy's fork: the play time counted with each frame's fraction above 30 FPS, the hit collector's cap (65,536), the save's invalid character id cleared | Ported (`InstallEngineFixes` in `bbport_guest_hooks.cpp`, from `bbgpu_patch_image`); `BB_FIX_PLAY_TIME=0`, `BB_FIX_HIT_COLLECTOR=0`, `BB_FIX_CHARA_ID=0` turn each off. Its light-grid setting is not taken |
| Point-light shadow wedges (each cube face cleared on its own), the grid of item icons over the HUD (stencil masks) | To check on Metal and port into the shared core |
| Occlusion samples counted only while the game's queries are open (about 0.8 ms of GPU time a frame on the PC) | Measured, not taken: three alternating pairs in Iosefka's Clinic, 39.1 against 38.9 FPS. On the PC the cost was waiting for the GPU to collect results; the Metal path does not wait for them |
| The frame limiter without catch-up slots, the FPS drop after loading another save | To check |
| Invert mouse X, a slow-walk key, skipping videos, the mouse auto-rotation off by default, *Exit to desktop* | To consider |
| Windows, the PC memory model only and heaps that grow, PCIe ring dedup and copy skip, NVIDIA/AMD specifics, DLSS, FSR 1 and FSR 4.1.2, FXAA/SMAA/GT-VBAO (Vulkan), BBSC, shader recipes from players' caches | Not applicable here, or covered otherwise (shaders ahead of time are N2.5) |

The plan with every item is the app's [roadmap](https://github.com/bloodborne-native/bloodborne-mac/blob/main/docs/ROADMAP.md).

## Other forks

| Source | Taken |
|---|---|
| [Huax-1839/bloodborne-macos](https://github.com/Huax-1839/bloodborne-macos/commit/0f0cf54) `0f0cf54`, adapted through Wooloomooloo2 | Direct Metal renderer: SPIR-V to MSL, shader rewrites, pipeline-state cache and format/state tables used in our Metal backend ([Native renderer](NATIVE_RENDERER.md)) |
| [Wooloomooloo2/bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port) `0b50589`, `78b59f5` | Further Metal integration: pipeline-key split, command layer and frame encoder; `4565225`: instanced tessellated draws one instance at a time. Its later Metal-native resources (`3ee456f`, "M2") were reviewed, not taken: the render adapter makes resources in Metal its own way. Lossless texture compression (MojahedAbushawish's lazy texture usages, `f151a8f`, #33), adapted: +33% FPS in Iosefka's Clinic (48.0–49.2 against 36.3–36.7) |
| [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) | Trophy event persistence (`79d44f0`); motion-vector pipelines skipped at cache preload (`f33e7617`); stable slot storage for images (`3a7cde8`); texture upload baselines, opt-in (`7e8bd6c1`, `136a72d0`); robustness fixes for stale texture formats, garbage PM4 headers, null vertex-buffer tables and early flip interrupts (`3e3b760`, `c07f0ff`, `2c4ddeb`, `76f5b5f`) |
| [bmy/bbport-mac](https://github.com/bmy/bbport-mac) | The Retina window; whole-image hashing of possibly changed textures; the MetalFX upscaler (`0333d60`), now the MoltenVK renderer's MetalFX path |
| [MaSieS4Fun/ARM_bloodborne_pc](https://github.com/MaSieS4Fun/ARM_bloodborne_pc) `3d56243`, [ducvd89/Bloodborne-Android](https://github.com/ducvd89/Bloodborne-Android) `bf1e6cf` | The portable shader-resource-table walker for non-x86 hosts; delayed condition-variable retirement; NV12 frame invalidation |

Skipped: the Windows fork's stretched-polygon fixes (bugs of its newer shadPS4 base),
`VK_EXT_device_fault` (MoltenVK lacks it), VRAM budget changes (unified memory). Authors and
licenses: [Credits](CREDITS.md).

## Taking a new bbport release

1. `git fetch upstream` (remote `https://github.com/deadinside28/bloodborne_pc`); bbport releases
   are squashed commits.
2. Merge or port the runtime, patches, scripts and game settings. Review renderer changes
   (`renderer_vulkan/`, `buffer_cache/`, `texture_cache/`, `shader_recompiler/` and their 0.6
   equivalents) one by one and port the useful ones into the shared core or the Vulkan backend.
3. Check what assumes Linux or native x86: no `MAP_FIXED_NOREPLACE`, `memfd_create`,
   `fallocate` or `gregs` on macOS (use `bb_shared_memory`, `BB_UC_*`, the runtime's low memory);
   the image is not at 0x800000000 (use `BB_IMAGE_BASE`); host code calls game functions through
   `runtime_guest_call`; x86 stubs run in bbcpu only from guest code ranges; libc++ needs complete
   types where GCC does not.
4. Build, run copied-save games, credit the work (commit message, [Credits](CREDITS.md), a code
   comment, the wiki) and update this page.
