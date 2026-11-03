# Shaders compiled during play

[← The PS4 layers](README.md)

The game's shaders are GCN code for the PS4's GPU. They are converted for the platform, and compiling
them at first use stalls the game. Converting them stays: the maintainer's decision is to convert
them once, ahead of time, rather than replace them by hand.

## What we did

- **A compile step** in the launcher's recompiler settings: off by default, it builds shaders and
  pipelines from the player's own game files, with nothing recorded and nothing shipped.
- **Pipelines from the engine's rules:** which targets, blends and vertex formats each pass uses.
  Model passes take their blends from the engine's own state tables: five material modes (opaque,
  alpha, multiply, add, subtract) in the group each pass's draw flags choose, and the point-light
  renderer's two fixed blends. The engine's own programs take the vertex layouts its renderers
  create them with (GXSimple, GUI, screen quads); Scaleform's take theirs from each input's name,
  read from the program's records, and the mesh formats Scaleform holds.
- **Shaders independent of the draw** where the source never read what differed.
- **Compute strides read at run time,** so a structure's size is no longer a shader form, and mixes
  of formats across a program's bindings.
- **What the pixel header says:** besides colour, whether a pixel program writes depth, stencil or
  the sample mask (the header holds DB_SHADER_CONTROL), so those shaders match play's, and they
  are planned only with a depth target.
- **The renderer's own kernels at start:** the tiling kernels of the tile modes the game uses and
  the indirect-dispatch guard are made when the renderer starts, not at their first use.
- **Tessellated draws:** the local, hull and domain programs pair by file name; what the game sets
  for the draw (the strides, the control points, the tessellation type) comes from their headers,
  and the effects' tessellated pipelines take their vertex layouts from the effects renderer's own
  tables. A domain shader declares, and on Metal reads, every patch constant its hull program
  writes, so both sides of the patch buffer have one layout.
- **One text per shader on Metal:** translations that differ only in numbering are one source.
- **No planned pipeline fails to build:** rect lists are planned only for vertex shaders that fetch
  no attributes, each pipeline gets the vertex translation made for its attributes' number types,
  and vertex shaders that keep a condition as a lane mask are rewritten for a one-lane wave (Metal
  has no SIMD-group functions in vertex functions). The step prints the reasons of any failure.

## Where it stands

After the step, 292 of the 300 shaders one session in Iosefka's Clinic made from the game's
programs were already made (97%): 146 of 149 fragment, 105 of 109 vertex, 34 of 35 compute and all
7 tessellation stages. The renderer's own 30 shaders (tiling, copies, upscaling, the overlays) are
made at start, before play. The 8 left read textures of another kind than the step assumes (a 1D
array, a 3D table) or attributes in other formats.

The step builds 2,407,516 pipelines and none fails (before: 656,734 of 3.3 million failed); every
planned pipeline has its shaders, and the translator stops on none of the game's programs. Of the
243 pipelines test sessions noted, tessellated draws included, the rules plan all 243.

From empty caches, a first play's worst frame is 561 ms, with 10 frames over 100 ms. That is one run,
standing still. With no step at all it was 1.9 s; with recorded states it is 258 ms. From empty
caches the step takes 103 s on an M3 Pro (one run): 15,242 shaders and 2.4 million pipelines.

## What is left

- The geometry stages (the models' cube passes and the particles' `.gpo` programs, with the export
  vertex programs before them), which the step does not translate yet.
- The port's own compute kernels, and the vertex and fragment forms still missed.
- Movement, combat and other areas, none of which are measured.
- Vulkan's build path: planning is shared, building is per backend.
