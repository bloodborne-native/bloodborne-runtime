# Architecture

[← Documentation](README.md)

How the runtime runs Bloodborne (PS4, CUSA03173, update 1.09) on a Mac: what each part does,
where its code is, and the switches that matter when working on it. Building and testing are in
[Building](BUILDING.md).

## Overview

```
eboot.bin ──► loader (src/probe.c) ──► game image in memory
                                         │
             game's x86-64 code ─────────┤  bbcpu (recomp/): translated to arm64 as it runs,
                                         │  or the recompiled library (BB_RECOMP_LIB)
                                         │
             PS4 system calls ───────────┤  runtime (src/runtime_*.c): memory, threads, files,
                                         │  saves, pad, audio, trophies, video out
                                         │
             GPU command buffers ────────┘  renderer (gpu/): PM4 packets and GCN shaders
                                            → Metal directly on Apple Silicon (the default)
                                            (or Vulkan → MoltenVK: BB_METAL_FRAME=0)
```

`run.sh` prepares a launch (checks the game folder, links the modules, writes patches, sizes
memory) and starts `out/bb-probe`. The launcher (bloodborne-launcher) sets its environment from
the player's settings.

## Loading the game

The game turns the dump's `eboot.bin` and its system modules into one flat image itself, in
memory (`src/runtime_prepare.c`, when `run.sh` starts `bb-probe` with `-`); the PS4's libc and
libSceFios2 are linked into it as game code, and the AppContent profile comes from `param.sfo`.
The Python scripts that did it before (`scripts/prepare.py`, `scripts/link_modules.py`,
`scripts/content_profile.py`) write the same bytes, and `bb-prepare` (`tools/bb_prepare.c`)
compares them. `src/probe.c` maps that image at `BB_IMAGE_BASE` (on macOS inside the guest address range that
`src/platform.c` reserves at startup), applies relocations and imports, then the patches
(built at start by `src/runtime_patches.c` from `patches/Bloodborne.xml`, the community's patch
collection, bbport.ini's choices and the player's patch folder: `--patches -`; `scripts/patches.py`
writes the same bytes, and `bb-patches` (`tools/bb_patches.c`) compares them), then the
runtime's own hooks (`bbgpu_patch_image`), and finally starts the game's entry point.

`scripts/game_check.py` refuses any executable other than 1.09: every address in the runtime is
an address in that executable.

## CPU: bbcpu and the recompiled library

The game's code is x86-64. On Apple Silicon it runs through **bbcpu**
([bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp), `recomp/src/cpu/`):
blocks are translated to arm64 the first time they run, with an interpreter as the fallback.
Guest memory is host memory (identity mapped). A guest call to an address outside the game's code
is a call of a host function, with the System V arguments bridged; host code calls game functions
with `runtime_guest_call` (`bbcpu_call` underneath). Details:
[Native arm64](https://github.com/bloodborne-native/bloodborne-recomp/blob/main/docs/ARM64_NATIVE.md).

`BB_RECOMP_LIB=recomp/out/recomp/librecomp.dylib` loads the **recompiled library** instead: the
game's functions generated as C from the player's own executable and compiled ahead of time. A
function whose bytes were changed by a patch or hook stays with the translator. The library is
built locally and never committed (it is game code). The app ships the recompiler's kit in
`Contents/Resources/game/recomp/` (`scan`, `bbrecomp`, the headers the generated C includes and
`build_library.py`, from CMake's `bb-recomp-*` targets), so the launcher can build the library on
the player's device with a downloaded LLVM toolchain. Details:
[Recompilation](https://github.com/bloodborne-native/bloodborne-recomp/blob/main/docs/RECOMPILATION.md).

## The PS4 runtime

`src/runtime_*.c` implements the system functions the game imports: memory
(`runtime_memory.c`), threads and locks, files (with mods layered over the game folder), saves,
the pad (SDL3 controllers and the keyboard), audio (AT9 through LibAtrac9), video playback
(AvPlayer on FFmpeg), the clock and trophies ([Trophies](TROPHIES.md)). The network and PSN
imports have offline answers here unless the online module is on (below). Platform differences
(Linux and macOS) are in `src/platform.{c,h}`.

**Online play** (`BB_ONLINE=1`; the launcher's Online page). `src/runtime_net.c` loads the online
module on the game's first network or PSN import and sends those imports to it; trophies, voice
and the store and profile dialogs stay with the runtime. On macOS the module is `libbbnet20`
(`gpu/bbnet20`: shadPS4 0.20's network, NP and shadNet code, with droogie/bbhost's backend for
The Hunter's Dream; GPL-3.0-or-later), in the app's Frameworks; on Linux it is bbport 0.51's
`libbbnet` (`gpu/bbnet`). It plays on two community pools: Seamless Hunt (shadNet) and The
Hunter's Dream (`BB_NET_BACKEND=thd`, after signing in from the launcher). Seamless co-op (BBSC)
is not supported. An edited save set plays offline only: the launcher refuses online play for it,
and `runtime_net_enabled` refuses it again when `savedata/.bb-edited` exists (or the marker or
`savedata` is a link or unreadable). If the module is missing or fails to start, the game plays
offline.

**Button prompts.** The game draws its key guides with PlayStation icons. The renderer knows those
icons and the HUD atlases that hold them by their bytes, hashed from the game's own files at start
(`gpu/shim/bbport_button_prompts.cpp`), and while the keyboard and mouse or a non-PlayStation pad
was used last it samples icons drawn by the port instead: the bound key or mouse button, or that
pad's button (`runtime_pad_prompt`; `vk_prompt_images.cpp`, the same on Metal and MoltenVK). No
game data is stored. `BB_BUTTON_PROMPTS=0`: the game's icons always.

**Memory.** Direct memory is 5,056 MiB as on the PS4. Outputs above 1080p need more for their
larger targets: `run.sh` sets `BB_DMEM_MB` to 5,056 MiB plus 448 bytes per scene pixel above
1080p, in 64 MiB steps. `BB_DMEM_EXTRA_MB` (512 by default) adds memory past the game's size that
only the runtime hands out (Laurence's render cache, below). Direct and flexible memory live in
one shared-memory pool that the GPU uses in place.

**Command buffers.** The game's main thread and its render thread write the same graphics command
buffer while an area loads. `src/runtime_cmdbuf.c` holds a lock per buffer around the game's
command-buffer methods (bbcpu guards; `BB_CB_GUARD=0`: off), and around the opening video's frame
and the render thread's task lists (`BB_MOVIE_STATE_GUARD=0`: off). `BB_CB_PROBE=1` names any
two threads that use one buffer at once.

## Hooks in the game's code

Some features change what the game does at a given instruction (from bbport 0.6:
`gpu/shim/bbport_frame_rate.cpp`, `bbport_game_hooks.cpp`). A hook replaces whole instructions
with a jump to a small stub `int3; the replaced instructions; jmp back`. bbcpu translates the stub
like the game's code; its `int3` reaches the runtime's handler with every register, which the
hook may change (`Core::InstallGuestCodeHook`, bbport's interface). Each site is checked byte for
byte first, so another game version leaves it alone. Code the runtime rewrites while the game
runs is excluded from the recompiled library.

| Hook | What it does | Off |
|---|---|---|
| Frame rate | Above 30 FPS the game's frame pacer and main loop step by each frame's real time, and 48 places tied to a 30 FPS frame follow it (physics timers, damping, cloth, the run's wall check). The limit applies live; **F3** toggles 30 FPS. | `BB_FRAME_HOOKS=0` (the community patches instead) |
| Effects | Chromatic aberration, depth of field, motion blur, SSAO (only where the game has its target), the game's anti-aliasing, dynamic-light shadows and bloom switch without a restart. Screen-space reflections stay a patch at start. | `BB_HOT_EFFECTS=0` |
| Aspect ratio | On an output that is not 16:9 the camera's aspect ratio follows it (wider sees more to the sides, taller more above and below), the HUD's markers stay over their characters, the UI's viewport has the output's shape and the HUD's groups move to the edges (Scaleform members set through the game's own functions). Full-screen UI fills reach the output's edges (a vertex-shader snap, `EmitUiEdgeStretch`). | `BB_HUD_ANCHOR=0` (HUD in the middle) |
| Mouse | Native mouse look: the mouse's turn is added to the camera's own turn each update (no stick dead zone, curve or speed limit); in lock-on and menus the mouse drives the right stick. A pointer in the game's menus (Scaleform lists): hover selects, left click confirms, right click goes back, the wheel scrolls (`gpu/games/bloodborne_menu_pointer.cpp`). | `BB_NATIVE_MOUSE=0`, `BB_MENU_POINTER=0` |
| Touch controls | On a touch screen (the iPhone, Android, a touch display) without a gamepad: on-screen sticks, face buttons, d-pad, L1/L2/R1/R2, L3/R3, Options, the touchpad and the settings menu, in the menu's colours, laid out in the safe area by the screen's height; a touch outside every control turns the camera from where it landed, the left stick sits where the thumb lands. Hidden while a gamepad is connected or the menu is open; taps are no mouse clicks while shown (`gpu/shim/bbport_touch_controls.cpp`, design after ducvd89/Bloodborne-Android). | `BB_TOUCH_CONTROLS=0` (`=1`: always) |
| Blur-kernel clamp | The blur kernel's base sample count raised to 3 (2 stalled the game: a black screen). | `BB_KERNEL_CLAMP=0` |
| Laurence's cinematic | Failed render jobs give their pool slot back; the render resource cache gets 512 MiB of the runtime's memory instead of 160 MiB. Above 30 FPS the cinematic hung or lost models without these. | `BB_JOB_SLOT_FIX=0`, `BB_RENDER_RESOURCE_CACHE_MB=160` |
| Resource copies | The GPU side hears of the game's resource uploads (new memory model only). | `BB_GUEST_HOOKS=0` |

`BB_GAME_HOOKS=0` turns off every hook but the frame rate's.

## Graphics

The renderer (`gpu/shadps4/`) is derived from [shadPS4](https://github.com/shadps4-emu/shadPS4)
and extended by bbport. It reads the PM4 packets the game writes into its command buffers,
translates the GCN shaders to SPIR-V. On macOS the native Metal adapter converts them to MSL
and draws directly on Metal. The selectable fallback uses
[MoltenVK](https://github.com/KhronosGroup/MoltenVK), loaded only for that run; Linux uses Vulkan.

**The draw pipeline.** Draws are prepared in two stages: stage A reads packets and resolves the
draw's resources, partly on preparation workers (`BB_PREP_WORKERS`); stage B, the GPU thread,
records them. `BB_DRAW_PIPE=0` runs both in one thread. Most of bbport's speed-ups can be switched
off at run time for bisection: `BB_TOGGLE_FILE` names a file holding a bit mask (re-read every
250 ms).

**Memory on the GPU.** The game's memory pool is used in place as GPU buffers, so the GPU reads
the game's data where it is. It is split into two overlapping windows (3,584 and 3,520 MiB) so no
binding offset reaches 4 GiB, where MoltenVK draws wrongly (`BufferCache::pool_upper`;
`BB_POOL_SPLIT=0`: one buffer). Native Metal wraps the same two windows itself
(`newBufferWithBytesNoCopy`); the other buffers and the images it makes directly in Metal. GPU
objects stay in a Metal residency set (`gpu/shim/bbport_metal_residency.*`); removed ones are
kept until the GPU has finished every submission that could use them.

**Resolution and upscaling.** The output has an aspect ratio (16:9 by default; 4:3, 16:10,
21:9, 32:9 or Auto, the monitor's) and a size from that ratio's list (`aspect_ratio`,
`output_res`). The game renders at 1920×1080 internally by default. A preset can make the scene
render smaller while the game's UI stays at output size. The upscaler is off by default. The
Metal renderer offers MetalFX only, encoded on the frame's command buffer (a setting asking for
FSR or DLSS gets MetalFX). The MoltenVK fallback offers FSR 3.1, FSR 4.1.1 and MetalFX (through
a bridge to the host's Metal device). The other route, live resolution, keeps the game at
1920×1080 and scales the renderer's targets while it runs (`live_resolution` in the settings).

**Character motion vectors** (`object_motion`, off by default on macOS): the game has no motion
vectors for moving characters, so a variant of their vertex shaders writes them, for the temporal
upscalers (sharper moving characters). They cost a few FPS with an upscaler.

**The Metal renderer** is the default on Apple Silicon (`BB_METAL_FRAME=1`). It owns the
device, queue, resources, synchronization, command buffers, pipelines and present; no Vulkan
loader or MoltenVK library is mapped. Its adapter still answers Vulkan-typed entry points from
the shared renderer. `BB_METAL_FRAME=0` selects the MoltenVK fallback, which loads the Vulkan
loader and ICD at run time. The launcher offers both until the
Metal backend is accepted (N5, step M4). Linux keeps its Vulkan backend. The adapter:
[Render adapter](RENDER_ADAPTER.md); the renderer's status and next steps:
[Native renderer](NATIVE_RENDERER.md#progress).

## Settings, the menu and the launcher

`gpu/shim/bbport_settings.*` holds the game settings (`bbport.ini`) and the launcher's own options
the menu shows (fullscreen, frame rate, renderer, ...). The launcher keeps the settings in its
database; while the game runs the two exchange changes as lines on the game's standard streams:
`BBSET key=value` (game settings) and `BBSET launcher:key=value`. The game reads them with
`read(2)` on its standard input.

The in-game menu (`gpu/shim/bbport_overlay.cpp`, Dear ImGui) opens with **F1** or **L3 + R3**. It
has the launcher's tabs and look, applies what it can at once, and offers a restart for the rest
(the launcher's options are exported to the environment again first). Its texts are the
launcher's locales (`gpu/shim/locales/`). The FPS counter's big size names the renderer and the
CPU path, and shows the game's CPU use (100% = one core) and the hottest die sensor in °C (Apple
platforms, from IOKit's HID event system on a thread of its own; the system's thermal state where
no sensor can be read).

Trophy unlocks are written to the profile's `trophies-v1.log`; the in-game overlay draws their
notices over the game's frame ([Trophies](TROPHIES.md)).

## Where it comes from

Almost everything that makes the game run (the loader, the PS4 runtime, the renderer's
extensions, the upscalers and the game patches) comes from
[bbport](https://github.com/deadinside28/bloodborne_pc), the Linux port this project started from,
and from shadPS4. This project adds the macOS platform layer, the arm64 translator and the
recompiler, the Metal work and the launcher integration. What came from which bbport release and
fork: [Upstream](UPSTREAM.md).
