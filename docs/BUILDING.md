# Building, running and testing

[← Documentation](README.md)

## Requirements

- macOS on Apple Silicon (macOS 15 or newer), or Linux with Nix (`shell.nix`; Linux is built in
  CI but not played on).
- macOS: Xcode Command Line Tools and [Homebrew](https://brew.sh):

  ```bash
  brew install bash pkgconf glslang nasm cmake ninja spirv-tools clang-format
  ```

  FSR 4.1.1, offered only with the MoltenVK renderer (`BB_METAL_FRAME=0`; it runs in Metal
  there), also needs Xcode with its Metal Toolchain (`xcodebuild -downloadComponent
  MetalToolchain`), even when `xcode-select` points at the Command Line Tools. Without it, or
  without `spirv-tools` or `clang-format` (DXC needs it), the build skips FSR 4.1.1 with a
  warning, and the game reports it unavailable. The default Metal renderer does not use it.

- Your own dump of the game with update 1.09, prepared as in
  [Game files](https://github.com/bloodborne-native/bloodborne-mac/blob/main/docs/guide/game-files.md).

## Build and run

```bash
git clone --recursive https://github.com/bloodborne-native/bloodborne-runtime.git
cd bloodborne-runtime
bash build.sh                                  # the first run also builds deps/ (about an hour)
BB_GAME_DIR=/path/to/your/game bash run.sh
```

- `build.sh` is a wrapper around `CMakeLists.txt`: one preset per configuration in
  `CMakePresets.json` (`macos-arm64`, `macos-arm64-debug`, `macos-arm64-asan`, `macos-x86_64`,
  `linux`), Ninja underneath. It builds `out/bb-probe`, the GPU library, bbcpu (from the `recomp/`
  submodule) and `bb-gpu-capabilities`; `bash build.sh --test` builds the tests and runs them with
  `ctest`. Everything is built into `build/<preset>/` and then copied to `out/`, which is where
  `run.sh`, the launcher and the test scripts look (the game's own files live there too).
  `cmake --preset macos-arm64-asan` and then `--build`/`ctest --preset` is the same build with the
  address and undefined sanitizers, `BB_SANITIZE` passes them by hand.
- `deps/` (MoltenVK, SDL3, FFmpeg, the online module's libraries, ...; for FSR 4.1.1 also DXC,
  the spirv-cross command and the fsr4-native-macos tree, without AMD's initializers) is built by
  `scripts/macos/build-deps.sh`, which runs `deps/CMakeLists.txt`. `deps/packages.cmake` has every
  version and the SHA256 of its archive; a package whose stamp is there is not touched, and
  bumping one rebuilds it alone. `cmake --build build/deps-arm64 --target deps-clean` installs
  everything again (the downloaded archives are kept). The packages are installed with `@rpath`
  ids and pkg-config files that find their prefix, so moving the checkout costs no rebuild.
  `deps/macos-<arch>` keeps each package's sources and build tree in `.build/` as well (a few GB):
  deleting that one directory is what makes everything install again, and nothing outside it
  decides what is installed.
- `run.sh` only runs the game and `build.sh` builds it: after pulling a checkout, one
  `bash build.sh` before `bash run.sh`. The test runners do it themselves (`tools/mac_bench.sh`,
  and through it `tools/test_run.sh` and the opening video test).
- A `deps/` from an earlier version is not reused: each package is keyed by its version and by the
  steps that build it, so the first build after updating installs all of them again (about an hour,
  the download is the same as before).
- The port's changes to FSR-Vulkan (`gpu/patches/`) are applied to a copy under the build
  directory, not to the submodule: the checkout keeps matching the submodule's commit. On macOS
  it is `fsr-vulkan-macos/0001-use-vulkan-headers-without-loader-on-macos.patch`, which links
  that library to the Vulkan headers while the native Metal path loads no Vulkan library.
- From a checkout, saves and caches go to `user/` and settings to `bbport.ini`.
- The macOS app (launcher, DMG, releases) is
  [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac), which builds this
  repository.

## Environment

`run.sh` reads its options from the environment; the launcher sets them from its settings.

| Variable | Effect |
|---|---|
| `BB_GAME_DIR` | The game folder |
| `BB_OUTPUT_RES=WxH`, `BB_RENDER_RES=WxH` | Output size, and the game's render size (with `BB_RENDER_RES`, also set `BB_DMEM_MB`) |
| `BB_UPSCALER=off\|fsr3\|fsr4\|fsr411\|taa\|dlss\|metalfx`, `BB_UPSCALE_PRESET` | Upscaler (off by default; an unknown value is off) and preset. The Metal renderer turns `fsr3`, `fsr4`, `fsr411` and `dlss` into `metalfx`; an FSR 4 the GPU cannot run falls back to `fsr3`, an unavailable DLSS to FSR 3.1 or MetalFX |
| `BB_FSR411_DIR` | AMD's FSR 4.1.1 files: the Vulkan path's set, or on macOS (MoltenVK) `initializers.bin` and `initializers-ultraperformance.bin` (`tools/fsr4_metal_initializers.sh <the player's amd_fidelityfx_upscaler_dx12.dll>`) |
| `BB_FSR4_METAL_SYNC=1`, `BB_FSR4_METAL_DIR` | FSR 4.1.1 on macOS with CPU waits instead of shared events; its built files (default `out/fsr4_metal`) |
| `BB_FPS=uncap\|90\|60\|30`, `BB_FPS_LIMIT` | Frame-rate mode and limit |
| `BB_METAL_FRAME=1`, `=0` | The Metal renderer (default on Apple Silicon) or the MoltenVK fallback; Rosetta and `BB_VK_DRIVER` always take MoltenVK/Vulkan |
| `BB_METAL_ARCHIVE=0` | No Metal binary archive: pipelines compiled when first used and not kept between runs (the iPhone app's setting: a loaded archive takes 13–37 times its size in address space) |
| `BB_SHADER_PREFETCH=0`, `BB_SHADER_AHEAD=1` | New shaders' Metal libraries compiled at first use instead of on the shader threads; shaders translated ahead of the GPU thread for the draw-preparation workers, opt-in (first-play stalls 12 s to 7 s; [Native renderer](NATIVE_RENDERER.md#n25-shaders-ahead-of-time)) |
| `BB_METAL_NATIVE_BOOT=1`, `=0` | Set by `run.sh` (and the launcher) with `BB_METAL_FRAME`: with `1` the window and device start without the Vulkan loader or MoltenVK |
| `BB_LAZY_TEXTURE_USAGE=0` | Every texture made with the ShaderWrite and PixelFormatView usages, as before lossless texture compression (default on: they are added once, with a copy, when a view needs one; MojahedAbushawish's lazy usages from bloodborne_mac_port) |
| `BB_GFX_BACKEND=metal`, `=vulkan` | The render adapter backend, set by `run.sh` with the renderer |
| `BB_METAL_CAPS=compare`, `=moltenvk` | `compare` logs every difference between the Metal backend's answers to the renderer's start-up capability queries and MoltenVK's (`Metal caps:` lines); `moltenvk` uses MoltenVK's answers |
| `BB_VULKAN_LOADER` | Path to the optional Vulkan loader on the MoltenVK path; `run.sh` sets it from `deps/` or the app bundle |
| `BB_RECOMP_LIB=PATH` | Load the recompiled library |
| `BB_ONLINE=1`, `BB_NET_BACKEND=thd` | Online play through the online module (Seamless Hunt; `thd`: The Hunter's Dream, with the launcher's sign-in). Refused for an edited save set ([Architecture](ARCHITECTURE.md#the-ps4-runtime)) |
| `BB_LANGUAGE`, `BB_UI_LANGUAGE` | The game's language (PS4 code), the menu's (`en`, `pt-BR`, ...) |
| `BB_MODS_DIR`, `BB_MODS_ENABLED=0` | Mods folder, or no mods |
| `BB_PATCHES_DIR` | Third-party patches folder |
| `BB_PATCHES_XML` | The game's patch list the runtime builds its patches from at start (default `patches/Bloodborne.xml`) |
| `BB_PROBE_PROGRAM` | The program `run.sh` asks for the scaled render and output sizes (`bb-probe --print-scaled SETTINGS`) when `BB_PROBE` wraps it in another command; default `out/bb-probe` |
| `BB_FRAME_STATS=1` | Frame statistics in the log |
| `BB_WAIT_STATS=1` | With the frame statistics, where the game's threads wait (mutexes, condition variables, semaphores, sleeps) by call site; off by default (it costs the waiting threads 3–10%); also on with `BB_WAIT_LOG=FILE` |
| `BB_THREAD_QOS=0` | The GPU command processor and the recorders at the default QoS class, as before (default: user-interactive, so Apple platforms keep them on the performance cores) |
| `BB_SKIP_GAME_CHECK=1` | Skip the game folder check |

Switches for single features are listed where the feature is described in
[Architecture](ARCHITECTURE.md).

## Tests

- `bash build.sh --test`: the native tests through `ctest` (settings, the AV player's clock and
  queue, stable slot storage, texture upload hashing, the runtime's services, ...). They need no
  game and no GPU. `ctest --preset macos-arm64` runs them again without rebuilding, `-R` picks one,
  and `ctest --preset macos-arm64 -N` lists them with the two that are disabled on macOS and why
  (guest TLS executes x86-64 code, the loader test builds 4 KiB pages; under `BB_SANITIZE` three
  more cannot run, because they map the guest's address space). Five GPU tests stopped compiling
  when the code moved under them and are not registered (`gpu/CMakeLists.txt` names them).
  Linux CI is started by hand for now; it compiles the Linux build but does not check its rendering.
- `cd tests && python3 -m unittest discover -p 'test_*.py'`: the script tests. On macOS the
  Linux-only cases fail (`test_packaged_vulkan`, the Intel tonemap check), and
  `test_probe.LoaderTests` fails (its images use 4 KiB segments; Macs have 16 KiB pages): known,
  not regressions.

## Running the game for tests

Tests use English (`BB_LANGUAGE=1 BB_UI_LANGUAGE=en`; the test scripts set it). Run one game at
a time (`pgrep -x bb-probe` first). Test windows open on the screen.

- `tools/test_run.sh NAME [VAR=VALUE...]`: a background run with a **copy** of the player's save
  (the real one is never written). It boots, loads the save, waits for the level (two frame-stat
  windows above `LEVEL_DRAWS=200` draws a frame), holds there (`HOLD=45` seconds) and prints the frame summary and any fault. `SAVE_FROM=DIR` starts from a launcher
  backup instead; `KEEP_SAVE=1` keeps the previous test's progress. The test copy of the
  settings is `out/test-user/bbport.ini` (keep `show_fps=1` and `fps_size=big` there).
  `TEST_TITLE="..."` (default: NAME) and `TEST_DETAIL="..."` put a banner at the top of the
  game's window saying what the run tests (`BB_TEST_TITLE`, `BB_TEST_DETAIL`: the overlay's
  test banner), for whoever sees it; it is in `grab.sh --screen` pictures, not in game frames.
  Each line is centered; under them a clock counts the run's time (light), then the time left
  once the hold starts (amber, red in the last 10 seconds; `BB_TEST_CLOCK`: a file where the runner writes when it will close the game, in seconds since
  1970). Each frame-stats window also logs `Process:`: the game's CPU time (100% = one core), the
  hottest die sensor and the system's thermal state.
- `tools/test_opening_video.sh NAME`: a new save in a fresh folder, through the opening video to
  character creation. Reports whether playback completed. `VIDEO_GRAB=1` (in the environment,
  not as an argument) saves the character creator's frame. Part of release testing.
- `tools/grab.sh [--screen] NAME`: saves the game's own frame as a PNG (`--screen` includes the
  in-game menu and the counter). Pictures come from the game, never from the screen.
- Test games are background-only apps (they never become the active app, so focus and Cmd+Q
  behave unlike a launcher game). For a test someone plays in, pass `BB_BENCH_FOREGROUND=1`.
- Scripted menu keys: `BB_MENU_KEYS_FILE=$PWD/out/menu.keys` with tokens like
  `r1 r1 down enter`; `touch out/menu.trigger` opens or closes the menu.
- `tools/mac_bench.sh`: what `test_run.sh` runs underneath (`SAMPLE=1` takes a CPU sample,
  `TRACE=1` a Metal System Trace).

## Measuring

The game is GPU-bound on Apple Silicon, so CPU-side changes are measured in CPU time. Compare
alternating runs at the same place with the same save and settings: FPS, frame-time p90, game
CPU (100% is one core) and the GPU thread's time per draw (`GPU thread us/draw` in the log).

A healthy run on the maintainer's M3 Pro (18 GB), with the defaults (the Metal renderer, native
1080p, no upscaler), reads 48.0–49.2 FPS standing in Iosefka's Clinic (about 1,660 draws a frame), with lossless
texture compression (`BB_LAZY_TEXTURE_USAGE`, adapted from [MojahedAbushawish](https://github.com/MojahedAbushawish)'s work for [bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port)).

Runs back to back are not equal: the second of a pair reads more GPU-thread time per draw as the
Mac warms up. Run the base first; when the new build reads worse, run one reversed pair (new
first) before calling it a cost.

Metal System Traces: `TRACE=1`, read with `tools/mst_summary.py`, `tools/mst_passes.py TRACE
--complete-frames` and `tools/mst_shaders.py`. `xctrace` needs
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`; recordings take over a minute to
finalize after their time limit.

What has been measured so far: [Performance](PERFORMANCE.md).

## Diagnostics

Off by default; their output stays in the ignored `out/` (it holds game addresses).

- Metal API validation (object lifetimes, invalid views): `MTL_DEBUG_LAYER=1
  MTL_DEBUG_LAYER_ERROR_MODE=nslog`, then search the log for "being destroyed while still
  required". The window's BGRA8 sRGB drawable warning is noise; two older reports are known
  ([Stability](STABILITY.md#open)).
- `BB_SHADER_LOG=1` (`vk_pipeline_cache.cpp`): one line per new shader variant (`Shader variant:`),
  with its conversion time; for a program's first variant what it is specialized for, for a
  later one which parts differ from the first. With an emptied test cache it shows what a first
  play converts ([Native renderer](NATIVE_RENDERER.md#n25-shaders-ahead-of-time)).
- `BB_DRAW_CENSUS=1` (`gpu/shim/bbport_draw_census.cpp`): counts the engine's draws at its render
  boundary (the functions named in bloodborne-recomp's `annotations/functions.tsv`) and every draw
  that reaches libGnm. Every 5 seconds it prints both counts and, for the draws that reach libGnm
  without passing the boundary, their most frequent return addresses; also the most frequent
  callers of the boundary's methods, the material classes the mesh draw uses, the materials'
  parameter objects, and how often the engine's and the middleware's locks are taken. It informs
  the renderer's step N6 ([Native renderer](NATIVE_RENDERER.md)).
- `BB_RENDER_BOUNDARY=observe` (`gpu/shim/bbport_render_boundary.cpp`; not with
  `BB_DRAW_CENSUS=1`): tags each draw made through the engine's draw methods with a NOP naming a
  record of its draw state, and the packet reader compares the record with the registers it
  decoded, and the record's resources with what the draw-preparation workers found the shaders
  read. Every 5 seconds: the methods' calls, the tagged draws, the draw packets with and without a
  record, per field how many agree, and per kind of resource how many were found; the first three
  differences of each field are printed. It costs frame rate. Nothing is
  drawn differently ([Native renderer](NATIVE_RENDERER.md#n6-the-engines-render-boundary)).
- `BB_METAL_CAPS=compare`: the Metal backend's capability answers against MoltenVK's (above).
- `BB_DEBUG_MOTION=1`: blends the camera-motion vectors over the frame.
- `BB_FRAME_STATS=1`: frame statistics in the log; `BB_CB_PROBE=1`: two threads using one
  command buffer at once.
- Crashes, translated code, threads and locks: [How to investigate](STABILITY.md#how-to-investigate).
