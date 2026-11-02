# PS4 textures and memory

[← The PS4 layers](README.md)

The PS4's CPU and GPU share one memory, and the game is written around that. It writes data and the
GPU reads it from the same place. Nothing tells the runtime what is a texture, so shadPS4's texture
cache infers it from GPU memory, and each texture exists twice: in the game's memory and as a GPU
texture, about 1-1.5 GB.

## What we did

- On Macs, whose memory is also shared, the GPU uses the game's memory pool in place.
- GPU-only textures are never evicted, which drew them dark.
- The recompiled library no longer keeps the game decoded: the peak footprint went from 7.8 GB to
  5.7 GB in Iosefka's Clinic.

The engine does track its textures itself: `TexRepository` is a hash table of reference-counted entries,
keyed by a path that ignores case and slash direction, and a texture can have an `_l` low-resolution
partner. `annotations/functions.tsv` in bloodborne-recomp names what is known of it, the repository's constructor, its
lookups and the Tpf repository beside it, and a class's size comes from the executable (`TexRepositoryImp`, 216
bytes). That is the place to take textures from. Texture objects are filled from a source object through virtual calls, in the
create fragment (`CSGXTextureCreateFragment`), and carry DXGI-style format codes; the GUI's textures map sRGB
formats to their linear ones the same way.

## Where it stands

0%: this has not started. Textures are still inferred and held twice.

## What is left

Taking textures as the objects the engine creates and releases, which removes the second copy.

This layer exists on every platform, and it costs more on PCs with a separate GPU memory, where shared
data has to be copied and kept in sync. Reading GPU results back there is slow, and the game assumes
it is cheap.
