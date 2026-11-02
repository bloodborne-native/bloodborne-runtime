# Documentation

[← Back to the project](../README.md)

**Status and next steps:** the renderer's in [Native renderer](NATIVE_RENDERER.md#the-layers)
(the only place they are kept); open crashes and corruption in [Stability](STABILITY.md#open).

Player guides (installation, game files, settings, mods, troubleshooting) are in the app,
[bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac/tree/main/docs).

- [Architecture](ARCHITECTURE.md): how the game runs: loading, the CPU, the PS4 runtime, online
  play, hooks in the game's code, the renderer, settings and the menu.
- [Building, running and testing](BUILDING.md): build, environment, test runs, measuring,
  diagnostics.
- [The PS4 layers](layers/README.md): what stands in for a PS4 in this repository, and what is left.
- [Performance](PERFORMANCE.md): what a frame costs on a Mac, what helped and what did not.
- [Stability](STABILITY.md): crashes and corruption fixed and still open, and how to investigate.
- [Native renderer](NATIVE_RENDERER.md): milestone the native renderer, drawing with each platform's own GPU API:
  progress, the Metal backend today, what comes next.
- [Render adapter](RENDER_ADAPTER.md): the specification of the adapter, the project's own GPU
  interface with Vulkan and Metal backends.
- [Local trophies](TROPHIES.md): recording, edited saves, notifications.
- [Upstream](UPSTREAM.md): what came from each bbport release and from other forks.
- [Credits and licenses](CREDITS.md)

The CPU layer and the recompiler: [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp).
