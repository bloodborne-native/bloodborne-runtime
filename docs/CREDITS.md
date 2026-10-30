# Credits and licenses

[← Documentation](README.md)

This project is licensed under the **GNU GPL v2 or later** ([LICENSE](../LICENSE)), like the
project it is based on.

## Based on

- [**deadinside28/bloodborne_pc**](https://github.com/deadinside28/bloodborne_pc) (bbport) by
  deadinside28: the native Linux port this project started from as a fork, and a separate
  repository since October 2026. Releases up to 0.51 are merged here with their history; parts
  of 0.6 are ported by hand (its renderer was rewritten). It provides the loader, the PS4
  runtime, the renderer extensions, the upscalers, the game-specific fixes and the original
  launcher. Code taken from it keeps its authorship in the git history; what came from which
  release is in [Upstream](UPSTREAM.md). From 0.6 (`8185b88`): the renderer fixes, the frame
  rate through hooks (`bbport_frame_rate.cpp`, `bbport_guest_code.h`), the game-code hook
  interface (`bbport_guest_code_hooks.h`, after shadPS4's InstallGuestCodeHook in the
  shadp2p/BBSC forks), Laurence's cinematic fixes, the live effects and the SSAO check
  (`bbport_game_hooks.cpp`, `bbport_effect_flags.h`), aspect ratios (camera, markers, UI viewport
  and HUD hooks, the UI edge stretch), the keyboard and mouse (`runtime_pad.c` and `window.cpp`
  merged from 0.6, `bbport_cursor.cpp`, native mouse look), the menu pointer
  (`gpu/games/bloodborne_menu_pointer.cpp`; its findings first published in a fork of bbport) and
  the button prompts for the device in use (`bbport_button_prompts.cpp`, `vk_prompt_images.cpp`).
  From 0.7 (`0fc2921`): the guard for GX occlusion queries without a handle (the fault at
  `0x2169edc`; whp199, bbport PR #343), as entry hooks in `bbport_guest_hooks.cpp`; the cloth
  stiffness per frame time above 30 FPS (bbport #58, `bbport_frame_rate.cpp`); the play time at high
  frame rates, the hit collector's cap and the save's character-id guard, which bbport took from
  [SrShadowy](https://github.com/SrShadowy)'s fork (`bbport_guest_hooks.cpp`).
- [**shadPS4**](https://github.com/shadps4-emu/shadPS4): the video core and shader recompiler
  (GPL-2.0+), and [sirit](https://github.com/shadps4-emu/sirit).
- **shadp2p** (a shadPS4 fork, GPL-2.0): the network and PSN libraries of the Linux online module
  (`gpu/bbnet`, shadNet), from bbport 0.51.
- **shadPS4 v0.20.0's network code** ([`529ceb3`](https://github.com/shadps4-emu/shadPS4/tree/529ceb32f5546b820588f194b903cc39d08015fe),
  GPL-2.0-or-later): the network, NP and shadNet libraries of the macOS online module
  (`gpu/bbnet20`), unchanged but for three marked changes.
- [**droogie/bbhost**](https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771)
  (GPL-3.0-or-later): the network side of its emulator, adapted as the online module's backend for
  The Hunter's Dream pool (`gpu/bbnet20/thd`). Its license makes the online module
  GPL-3.0-or-later (`gpu/bbnet20/LICENSE.GPL-3.0`, also in the app).

## From other forks

Changes taken from other ports of bbport, with thanks:

- [**Huax-1839/bloodborne-macos**](https://github.com/Huax-1839/bloodborne-macos/commit/0f0cf54)
  (GPL-2.0-or-later, `0f0cf54`, Huax-1839's first public code snapshot): the direct Metal
  renderer that supplied the SPIR-V to MSL shader compiler, the rect-list, tessellation and
  geometry-shader rewrites, the pipeline-state cache and the Vulkan-to-Metal format and state
  tables in `video_core/renderer_metal/`, adapted here through Wooloomooloo2's port.
- [**Wooloomooloo2/bloodborne_mac_port**](https://github.com/Wooloomooloo2/bloodborne_mac_port)
  (GPL-2.0-or-later): integrated and developed that Metal code further, including the pipeline-key
  split, shared blend rules, shadow check, Vulkan-to-Metal command layer and frame encoder
  ([Native renderer](NATIVE_RENDERER.md)); from `4565225`, instanced tessellated draws one
  instance at a time. Our imported snapshot is `0b50589`, with `78b59f5` also adapted. Lossless
  texture compression from
  [**MojahedAbushawish**](https://github.com/MojahedAbushawish)'s lazy texture usages there
  (`f151a8f`, #33, merged by Mark Hall in `4fcca1d`): textures are made without the ShaderWrite
  and PixelFormatView usages until a view needs one, adapted to this renderer's images
  (`VideoCore::Image::allowed_storage`).
- [**Supermedo/bloodborne_pc**](https://github.com/Supermedo/bloodborne_pc) (GPL-2.0-or-later),
  the Windows fork:
  - `79d44f0` (Ambrose): per-user trophy event persistence (`src/runtime_trophies.c`), adapted
    with a versioned, title-aware log, checked file operations and edited-save eligibility; its
    launcher's platinum and base/DLC trophy grouping, cross-checked against public trophy lists.
    No trophy files, artwork or keys were taken; the trophy catalog's public names and original
    summaries are credited in the launcher.
  - `f33e7617` (João Pedro Valadares): motion-vector graphics pipelines skipped at cache preload,
    extending the existing motion-shader rejection to keys whose cached metadata describes a
    plain permutation; they rebuild at first use.
  - `7e8bd6c1` (devilismyfriend, committed by ayozetr) with `136a72d0` (Supermedo): guest-memory
    baselines recorded before GPU image writes, without hashing large streamed textures, against
    stale CPU uploads; adapted with explicit baseline validity, independent mip state and
    backing-view reads in precise-readback mode.
  - `3a7cde8` (João Pedro Valadares, after upstream shadPS4): stable `SlotVector` storage and
    texture-cache reference reacquisition; adapted with POSIX page protection inside the reserved
    mapping, fatal allocation checks and a regression test.
  - Robustness fixes from 1.7.7–1.8.2, each written anew there from community pull requests: a
    stale texture format no longer stops the game (`3e3b760`, idea from parsifal295, PR #55); a
    garbage PM4 header drops the rest of its buffer (`c07f0ff`, parsifal295, PR #57); a draw whose
    vertex buffer table pointer is null is skipped (`2c4ddeb`); a flip IRQ ahead of its label
    waits briefly instead of stopping the game (`76f5b5f`, from Awaken's port).
  - Windows 1.8 and 1.8.1 ("Awaken"): the first to put shadPS4 0.20's network code and bbhost's
    backend together in a bbport build, which the online module follows.
- [**bmy/bbport-mac**](https://github.com/bmy/bbport-mac) (GPL-2.0-or-later; a Rosetta 2 port of
  bbport to macOS):
  - the Retina-resolution window (`gpu/shim/window.cpp`, `BB_RETINA`);
  - the texture cache hashing whole images when checking a possibly changed one (the item
    picture on the loading screen);
  - the MetalFX temporal upscaler ([`0333d60`](https://github.com/bmy/bbport-mac/commit/0333d6004fd1499d6b4d029a3e6e5dd3c511f8ea):
    `video_core/renderer_vulkan/vk_metalfx.*`, `shim/bbport_metalfx.*`): the bridge to the host's
    `MTLDevice` through `VK_EXT_external_memory_metal` and the submission-thread dispatch, adapted
    to this tree (settings, menu, scheduler). It is the MoltenVK renderer's MetalFX path; the
    Metal renderer's MetalFX, encoded on its own frame's command buffer in the same files, is
    this project's code.
- [**MaSieS4Fun/ARM_bloodborne_pc**](https://github.com/MaSieS4Fun/ARM_bloodborne_pc/commit/3d56243)
  (GPL-2.0, `3d56243`): the portable SRT walker and its `SrtOp` program for non-x86 hosts.
- [**ducvd89/Bloodborne-Android**](https://github.com/ducvd89/Bloodborne-Android/commit/bf1e6cf)
  (GPL-2.0, `bf1e6cf`): carried that walker to bbport 0.51 with shader-cache serialization and
  host-architecture separation; also supplied delayed condition-variable retirement and
  pitch-sized NV12 frame invalidation. Only these runtime changes were taken.
- [**maze1377**](https://github.com/maze1377)'s port of our texture-collector fix to
  [**SpartanPort**](https://github.com/FelipePrado0/SpartanPort/pull/1124) (GPL-2.0, PR #1124,
  `8f463a1`): GpuDirty images are not among the GPU-written images the collector keeps, their
  data being in the buffer cache (`TextureCache::GarbageCollectImages`); the port also found that
  our release of those images over the critical mark mostly did not happen.

## Libraries

- [MoltenVK](https://github.com/KhronosGroup/MoltenVK) (Apache-2.0). The Metal backend's answers to the renderer's start-up Vulkan queries (`renderer_metal/mtl_vk_caps_profile.inc`) are MoltenVK 1.4.2's answers on an Apple M3 Pro, recorded with `BB_METAL_CAPS=compare` (`tools/vk_caps_profile.py`).
- [Zydis](https://github.com/zyantific/zydis) (MIT), the translator's x86 decoder
- [SDL3](https://www.libsdl.org/) (zlib)
- [FFmpeg](https://ffmpeg.org/) (LGPL)
- [Mesa](https://www.mesa3d.org/) KosmicKrisp (MIT, optional)
- [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn and the AMD FidelityFX SDK (MIT).
  The macOS build applies a local CMake patch in `gpu/patches/fsr-vulkan-macos/` so this library
  uses Vulkan headers without linking the loader. The upstream submodule commit is unchanged.
- [fsr4-native-macos](https://github.com/UzenUPoZiTiV4ik/fsr4-native-macos) by PoZiTiV4ik (UzenUPoZiTiV4ik) (MIT), FSR 4.1.1 in Metal for the
  MoltenVK renderer
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT)
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT)
- [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) (Apache-2.0), the Metal backend's shader conversion
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) by Yuji Hirose (MIT), in the online module
- The macOS online module's libraries, linked statically (`deps/packages.cmake`): Abseil and
  OpenSSL (Apache-2.0), Protobuf (BSD-3-Clause), nlohmann_json and epoll-shim (MIT), miniupnpc
  (BSD-3-Clause)
- [half](https://half.sourceforge.net/) (MIT)
- DejaVu fonts
- [Inter](https://github.com/rsms/inter) by Rasmus Andersson and [Cinzel](https://github.com/NDISCOVER/Cinzel)
  by Natanael Gama (SIL Open Font License 1.1), the in-game menu's text and headings, as in the
  launcher
- The launcher's libraries (Tauri, React and others) are credited in
  [bloodborne-launcher](https://github.com/bloodborne-native/bloodborne-launcher).

## Game patches

By Kyo, Lance McDonald, auser1337, illusion, emoose and other community members
(`patches/Bloodborne.xml`).

## Disclaimer

This project is not affiliated with shadPS4, Sony Interactive Entertainment, FromSoftware or AMD.
*Bloodborne* is a trademark of Sony Interactive Entertainment. No game files are included.

