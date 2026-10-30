# bloodborne-runtime

Runs Bloodborne (PS4, CUSA03173 1.09) outside a PS4: the loader, the PS4 system libraries
(`src/runtime_*.c`), the optional online module (`gpu/bbnet20` on macOS, `BB_ONLINE=1`) and the
renderer (`gpu/`, derived from shadPS4; on Apple Silicon Metal directly by default, or Vulkan
through MoltenVK with `BB_METAL_FRAME=0`; Vulkan elsewhere). Built on bbport. The game's code
runs through bbcpu from the `recomp/` submodule (bloodborne-recomp), or as its recompiled library
(`BB_RECOMP_LIB`).

## Read first

- `docs/NATIVE_RENDERER.md`: the renderer's status and next steps (its Progress table is the only
  place they are kept; milestone R5).
- `docs/STABILITY.md`: the open crashes and corruption, and what was fixed and why. Read it
  before chasing a crash or corruption.
- `docs/ARCHITECTURE.md`: how it works, with the switches of each part.
- `docs/BUILDING.md`: build, environment, test runs, measuring, diagnostics.
- `docs/PERFORMANCE.md`: what helped and what did not. Read it before trying a performance idea.
- `docs/RENDER_ADAPTER.md` (the adapter's specification), `docs/TROPHIES.md`,
  `docs/UPSTREAM.md` (bbport and forks), `docs/CREDITS.md`.

## Layout

- `src/probe.c` (the program, `out/bb-probe`), `src/runtime*.c`, `src/platform.{c,h}` (Linux and
  macOS differences).
- `gpu/`: `shadps4/` (video core, shader recompiler; `video_core/renderer_metal/` the Metal
  backend), `shim/` (window, in-game menu, settings, game hooks, Metal residency), `layer/`,
  `cp/`, `games/` (game profile). Built by CMake into `libbbgpu`.
- `src/runtime_prepare.c`: the executable prepared and linked in memory at start (`bb-probe -`);
  `src/runtime_patches.c`: the game's patches built at start (`--patches -`).
- `scripts/`: `game_check.py`, `mods.py` (at start); `prepare.py`, `link_modules.py`, `patches.py` (the same bytes as files, for checks);
  `scripts/macos/build-deps.sh`.
- `run.sh` (the app calls it), `build.sh`, `patches/Bloodborne.xml`, `tools/` (test runners,
  `grab.sh`, Metal trace readers).

## Build and test

```sh
bash build.sh [--test]                      # preset + ninja; deps/ the first time (~1 h)
cmake --preset macos-arm64-asan             # the same with address,undefined (or macos-arm64-debug)
cmake --build --preset macos-arm64 --target bb-tests && ctest --preset macos-arm64
tools/test_run.sh NAME [VAR=VALUE...]       # background run with a copy of the player's save
tools/test_opening_video.sh NAME            # new save through the opening video
cd tests && python3 -m unittest discover -p 'test_*.py'
```

On macOS the Linux-only Python tests (`test_packaged_vulkan`, the Intel tonemap check) and
`test_probe.LoaderTests` fail: known. A healthy run on the maintainer's M3 Pro with the defaults
(Metal renderer, native 1080p, no upscaler) reads 48.0–49.2 FPS standing in Iosefka's Clinic (with lossless texture
compression, adapted from MojahedAbushawish's work for bloodborne_mac_port); FPS
is bound by rendering, so CPU-side changes are measured in CPU time, in alternating runs
(`docs/BUILDING.md#measuring`).

## Rules for this repository

- Every address in the runtime is an address in the 1.09 executable; hooks and patches check
  their bytes first.
- Hooks in the game's code go through `Core::InstallGuestCodeHook` (`gpu/shim/
  bbport_guest_code_hooks.h`) or stubs bbcpu runs from guest code ranges; host code calls game
  functions with `runtime_guest_call`. Code rewritten while the game runs is excluded from the
  recompiled library (`bbcpu_recomp_exclude`). Install hooks from `bbgpu_patch_image`, before the
  recompiled library is checked.
- MoltenVK draws wrongly from buffer offsets past 4 GiB: any new pool-sized buffer is split the
  way `BufferCache::pool_upper` splits the guest pool.
- GPU objects may be freed only after every submission that could use them has completed
  (residency set, texture-cache deletions, staging buffers). Metal API validation finds
  violations (`docs/BUILDING.md#diagnostics`).
- Do not turn off MoltenVK's Metal argument buffers: ~85% slower here.
- Trophies stay local, with or without the online module; check `.bb-edited` in the selected
  save before recording; a launcher flag cannot re-enable an edited save; earned history
  survives restores. An edited save set plays offline only (the maintainer's decision): the
  launcher refuses online play and `src/runtime_net.c` refuses it again; keep both checks.
- New bbport releases: `docs/UPSTREAM.md` ("Taking a new bbport release"). Record what was
  ported there and in `docs/CREDITS.md`.
- No game data in the repository: captures, traces, frame dumps and logs with addresses stay in
  the ignored `out/`.

## Working rules (all four repositories)

- **The project is four repositories**, nested as submodules:
  [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) (the app) ⊃ `runtime/` =
  [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime) ⊃ `recomp/` =
  [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp); the app also has
  `launcher/` = [bloodborne-launcher](https://github.com/bloodborne-native/bloodborne-launcher).
  Commit in the repository that owns the file, push it, then bump the submodule pointer in the
  parent and push that too. Submodules are checked out at a commit: run `git checkout main` (and
  `git pull`) there before editing.
- **Before any push, check `git remote get-url origin`.** Push to `main`; ask before
  force-pushing anything that existed before this session.
- **Commits:** a short subject, a body that says what changed and why (numbers when measured),
  and the trailer the session asks for.
- **Credits:** built on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28
  and on [shadPS4](https://github.com/shadps4-emu/shadPS4); keep them credited. Code taken from
  another project or fork is credited in the same commit: CREDITS.md, the wiki's credits page,
  the commit message and a short code comment.
- **No game code, ever:** no decompiled or generated game code, no game files, no records of game
  memory in any repository or wiki.
- **Testing the game:** `tools/test_run.sh` (a copy of the save; the real one is never written),
  in English, one game at a time (`pgrep -x bb-probe` first). Test windows open on the user's
  screen: say so before long batches, and stop when asked.
- **Every test run says what it tests, on screen:** `TEST_TITLE="..." TEST_DETAIL="..."
  tools/test_run.sh NAME ...` puts a banner at the top of the game's window (the title defaults to
  NAME). The title names the test and the build (e.g. "M2 skeleton: new build"); the detail
  says the settings and what is being checked. Never start a test window without them.
- **Never capture the user's screen.** Pictures come from the game: `tools/grab.sh`.
- **Shell:** no `rm -rf` on variable paths; no `xargs ... sh -c '...'`.
- **Docs:** plain, precise English; numbers measured, not guessed. Docs say what is true now;
  investigation logs do not go in docs (conclusions do). The wiki mirrors the docs; update both.
