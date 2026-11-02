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
heap with the last reference. Textures take their pixels from `GFX_GraphicsPrivateMix` unless a flag in their description or the caller says otherwise. Render targets take the same default: at least 21 of the 25 calls to `CG::RenderTarget::create` pass no heap.

The game's heaps have fixed sizes, in a table of the executable with a row per build. The shipped one, `Master_ps4_GFXMIX`, adds up to 4,478 MiB, and
`Master_ps4_flexible` adds 162 MiB:

| Heap | MiB |
|---|---:|
| `GFX_GraphicsPrivate` | 2,287 |
| `GFX_GraphicsPrivateB` | 886 |
| `MAIN` | 524 |
| `HAVOK` | 296 |
| `GFX` | 276 |
| `GFX_SystemShared` | 152 |
| `MENU` (flexible) | 82 |
| `CORERES` (flexible) | 48 |
| `DEBUG` (flexible) | 32 |
| Others | 57 |

`GFX_GraphicsPrivateMix` joins the two private heaps, 3,173 MiB, so textures that take this default fit in that on a PS4. Render targets are made by the same call and share it unless their caller passes heaps; whether callers do has not been read yet.
 The graphics device also keeps live totals of what it holds, six sections of images `0x38` bytes apart, in
order 2D textures, cube textures, render targets, depth targets and two more, and one for what look like shaders: a resource joins its section when it
is created and leaves when destroyed, its size added to a running total in bytes, kept beside the count and the
total in MiB, so the game itself knows how much it holds (an image keeps its size in bytes at `+0x58`). Textures come from file resources: character, parts and model packs, loose `.tpf` files and start-up data. A
map loads in tiers, each a run of steps in `MapRes`'s state machine: the model, then `LoTex`, `LoHiTex` and `HiTex`,
each with `_Init`, `_Wait`, `_Finish` and `_Release`, and the `HiTex` step takes its holds through
`TexRepository::findOrCreate`. Each tier's `_Init` step opens its textures through `MapTextureSet::load`, as
`tpfbnd:/name.tpf` for each texture entry of the model: `LoTex` fills one set from each entry's second name, and `LoHiTex` and
`HiTex` the other from its first, so an entry names a low and a high version of the texture. So the game streams map textures by tier and releases them itself. The level comes
from `MapInsUpdateHelper::update`, not from distance: it walks the map's pieces and compares each one's group masks
with the current ones, changing a limited number on each call. A piece in the current groups gets its full level,
one in a second set the low level, and the rest are released, and a queue applies the changes: pieces with work wait in a list, which runs their steps each frame and drops each one when it is done. Releases do not happen at once: the world keeps a list of up to 1,000 of them, for held textures and
file resources, and works through at most ten on a frame, starting from one and doubling while it stays busy. When the list is full the oldest runs at once, so textures
a piece gave up stay in memory for a little while after it leaves. So what is in memory
follows those masks, which the game calls backread (its debug page is `WorldBackRead`). They do not come from the camera. `WorldBackRead::update` runs a navmesh search from the player's
position, again after 5 m of movement or a change of navmesh cell, out to the larger of its two ranges. Each collision part found enters two
timed lists, and while one is active `WorldGroups::addCollisionGroups` adds its groups, and the groups it connects to in other map
blocks, to one mask per block (60 blocks). `WorldGroups::update` then writes the masks to collision parts, map pieces and
characters. Characters use them too: `WorldChrMan::updateBackread` sets each one's state from the masks, the nearest first, up to a limit. So a map's textures stay by connection between areas, not by what is on screen. `Sprj::World::update` hands it the player's position on every frame, along with the collision part the player stands on. How often that happens in play has not been measured. So the engine's own texture lifetime passes through
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

GPU memory for render data comes from a heap (`CG::GpuHeap`). Freeing is deferred: a block goes on a retired list and an end-of-pipe write marks it, and the heap reuses it only when it next runs short (`reclaimRetired`).

The SDK's texture layout code is linked into the executable and named: the `Gnm::Texture`, `RenderTarget` and `DepthRenderTarget` field readers, and `GpuAddress` with its tile-mode table, surface sizes, and tilers for linear, 1D and 2D layouts. One copy of `GpuAddress` belongs to YEBIS's Direct3D 11 layer, which tiles and detiles the surfaces it uploads; a texture written that way is already in the console's tiled layout when the GPU sees it.
