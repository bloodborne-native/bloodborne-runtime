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
bytes). Every resource goes through the same base, `FD4ResCapHolder`, with two counts: one at `+0x60` and a hold at
`+0x64`, which map, object and material code release. A resource leaves the holder's table, keyed by the same
hash of its name at `+0x18`, only when both are clear. A texture with both clear is destroyed; one with no
references left but still held stays, with its handle pointed at the default texture, the one `TexResCap` uses
when given none. The handle drops its reference to the old image, and the last reference to go runs the image's
destructor, so a held texture keeps its name and gives up its image. Creation comes back to that state on purpose: `TexRepository::createBatch` takes a list of
file entries, makes one image object per kind of texture on the work pool, waits for them and sets them on
their handles, and for a name it finds with no references but still held it builds the image again in the same
entry. So the engine unloads textures and restores them itself. The unload starts when a `.tpf` file resource goes: `TpfResCap`'s destructor releases
each texture it made, and an image's destructor drops a shared storage object at `+0x40`, which goes back to its
heap with the last reference. The graphics device also keeps live totals of what it holds, six sections of images `0x38` bytes apart, in
order 2D textures, cube textures, render targets, depth targets and two more, and one for what look like shaders: a resource joins its section when it
is created and leaves when destroyed, its size added to a running total in bytes, kept beside the count and the
total in MiB, so the game itself knows how much it holds (an image keeps its size in bytes at `+0x58`). Textures come from file resources: character, parts and model packs, loose `.tpf` files and start-up data. A
map loads in tiers, each a run of steps in `MapRes`'s state machine: the model, then `LoTex`, `LoHiTex` and `HiTex`,
each with `_Init`, `_Wait`, `_Finish` and `_Release`, and the `HiTex` step takes its holds through
`TexRepository::findOrCreate`. So the game streams map textures by tier and releases them itself. The level comes
from `MapInsUpdateHelper::update`, not from distance: it walks the map's pieces and compares each one's group masks
with the current ones, changing a limited number on each call. A piece in the current groups gets its full level,
one in a second set the low level, and the rest are released, so what is in memory follows the groups in view. How often that happens in play has not been measured. So the engine's own texture lifetime passes through
one place. That is the place to take textures from. Texture objects are filled from a source object through virtual calls, in the
create fragment (`CSGXTextureCreateFragment`), and carry DXGI format codes: the engine's graphics layer is shaped like Direct3D 11, with a device that
creates buffers and 2D textures from descriptions (size, mips, format, bind flags) and shader resource, render
target, depth stencil and unordered access views onto them; the GUI's textures map sRGB
formats to their linear ones the same way.

## Where it stands

0%: this has not started. Textures are still inferred and held twice.

## What is left

Taking textures as the objects the engine creates and releases, which removes the second copy.

This layer exists on every platform, and it costs more on PCs with a separate GPU memory, where shared
data has to be copied and kept in sync. Reading GPU results back there is slow, and the game assumes
it is cheap.
