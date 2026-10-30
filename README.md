# bloodborne-runtime

[![Discord](https://img.shields.io/badge/Discord-join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/QS6vHz45xA)

What runs Bloodborne (PS4, CUSA03173 1.09) outside a PS4: it loads the game's executable,
provides the PS4 system libraries the game calls (threads, memory, files, saves, pad, audio,
video out, network) and draws its graphics with a renderer derived from shadPS4. On Apple
Silicon the renderer draws with **Metal directly** (the default; MetalFX is its upscaler); the
older path through Vulkan and MoltenVK stays selectable (`BB_METAL_FRAME=0`, with FSR 3.1);
FSR 4.1.x runs on both renderers. Other platforms use Vulkan. An optional online module (`BB_ONLINE=1`) plays on the
community servers: Seamless Hunt and The Hunter's Dream.

It is **built on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28**, the
native Linux port this work started from as a fork, whose releases keep being merged or ported
here (0.51, and parts of 0.6: [Upstream](docs/UPSTREAM.md)). On Apple Silicon the game's code
runs through bbcpu, the CPU layer of
[bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp) (the `recomp/`
submodule), or as recompiled native code it generates.

| Repository | |
|---|---|
| [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp) | The recompiler and bbcpu: portable |
| **bloodborne-runtime** (this one) | Loads the game: the PS4 libraries, the renderer |
| [bloodborne-launcher](https://github.com/bloodborne-native/bloodborne-launcher) | The independent launcher: profiles, saves and settings |
| [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) | Packages the launcher as the macOS app, with releases and player guides |

**Players:** download the app from [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac/releases/latest).

## Status

- Native Metal is the default on Apple Silicon since the app's 0.9.0 RC 1, and accepted (N5,
  2026-10-29); MoltenVK stays selectable as a fallback. The online module ships from 0.9.0 RC 2.
- The renderer's progress and next steps: [Native renderer](docs/NATIVE_RENDERER.md#progress).
- Known crashes and corruption, fixed and open: [Stability](docs/STABILITY.md#open).
- The app's releases, its roadmap and what players report:
  [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac/blob/main/docs/STATUS.md).

## Build and run

```bash
git clone --recursive https://github.com/bloodborne-native/bloodborne-runtime.git
cd bloodborne-runtime
bash build.sh                                  # macOS: the first run also builds deps/
BB_GAME_DIR=/path/to/your/game bash run.sh     # your own dump, with the 1.09 update
```

[Building](docs/BUILDING.md) · [Architecture](docs/ARCHITECTURE.md) ·
[Documentation](docs/README.md) · [Credits and licenses](docs/CREDITS.md)

## Credits

The loader, the PS4 runtime, the renderer's extensions, the upscalers and the game-specific fixes
come from [bbport](https://github.com/deadinside28/bloodborne_pc); the video core and shader
recompiler from [shadPS4](https://github.com/shadps4-emu/shadPS4), and the online module's
network code from shadPS4 0.20 and [droogie/bbhost](https://github.com/droogie/bbhost). The Metal
backend adapts shader and pipeline work from
[Huax-1839/bloodborne-macos](https://github.com/Huax-1839/bloodborne-macos) through
[Wooloomooloo2/bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port),
which developed the command layer and frame encoder further. The macOS platform layer, the arm64
path and the render adapter were added here. Full list:
[Credits and licenses](docs/CREDITS.md).

Licensed under the [GNU GPL v2 or later](LICENSE); the online module is GPL-3.0-or-later (it
includes bbhost's code). Not affiliated with shadPS4, Sony Interactive Entertainment,
FromSoftware or AMD. *Bloodborne* is a trademark of Sony Interactive Entertainment. No game files
are included.
