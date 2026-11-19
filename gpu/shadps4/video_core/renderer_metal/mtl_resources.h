// SPDX-License-Identifier: GPL-2.0-or-later
// The render adapter's Metal resources (docs/RENDER_ADAPTER.md, M1): with BB_GFX_BACKEND=metal,
// the device makes buffers in Metal and hands them to MoltenVK (VK_EXT_metal_objects imports).
// Elsewhere these are no-ops.

#pragma once

#include "common/types.h"

namespace MetalResources {

enum class Storage : u8 {
    Private,           ///< GPU only
    SharedWriteCombined, ///< CPU writes, GPU reads
    Shared,            ///< CPU reads and writes
};

/// A Metal buffer (id<MTLBuffer>, retained); `contents` is null for private storage.
struct Buffer {
    void* object{};
    void* contents{};
};

/// A texture as a Vulkan image describes it (`vk_format`: a VkFormat; `type`: 1, 2 or 3
/// dimensions).
struct TextureDesc {
    u32 vk_format{};
    u32 type{2};
    u32 width{1}, height{1}, depth{1};
    u32 levels{1}, layers{1}, samples{1};
    bool shader_read{}, shader_write{}, render_target{};
    bool format_views{}; ///< views in other formats or of another type
};

/// A Metal texture (id<MTLTexture>, retained, private storage); `object` is null when Metal has
/// no texture exactly like the Vulkan image (its format substituted or swizzled).
struct Texture {
    void* object{};
    u64 size_bytes{};
};

#ifdef BB_METAL_BACKEND
/// BB_GFX_BACKEND=metal on the Metal build: buffers are made in Metal.
bool Enabled();
/// MoltenVK's Metal device, from `device` (a VkDevice); before the first NewBuffer.
void Init(void* device);
/// A buffer of `size` bytes; fatal when Metal cannot make it.
Buffer NewBuffer(u64 size, Storage storage);
/// A buffer over `size` bytes of host memory at `host` (both page-aligned), used in place; the
/// memory stays the caller's (M4: the guest memory pool).
Buffer WrapHostMemory(void* host, u64 size);
Texture NewTexture(const TextureDesc& desc);
/// Gives up a reference taken by NewBuffer or NewTexture.
void Release(void* object);
/// The textures NewTexture made that are not released yet, and their bytes.
void LiveTextures(u64* count, u64* bytes);
/// Buffers made by NewBuffer and WrapHostMemory and not released yet, by kind: private,
/// shared, shared write-combined, host memory used in place. Counts and allocated bytes.
constexpr int kBufferKinds = 4;
void LiveBuffers(u64 counts[kBufferKinds], u64 bytes[kBufferKinds]);
#else
inline bool Enabled() {
    return false;
}
inline void Init(void*) {}
inline Buffer NewBuffer(u64, Storage) {
    return {};
}
inline Buffer WrapHostMemory(void*, u64) {
    return {};
}
inline Texture NewTexture(const TextureDesc&) {
    return {};
}
inline void Release(void*) {}
inline void LiveTextures(u64* count, u64* bytes) {
    *count = *bytes = 0;
}
constexpr int kBufferKinds = 4;
inline void LiveBuffers(u64 counts[kBufferKinds], u64 bytes[kBufferKinds]) {
    for (int k = 0; k < kBufferKinds; ++k) counts[k] = bytes[k] = 0;
}
#endif

} // namespace MetalResources
