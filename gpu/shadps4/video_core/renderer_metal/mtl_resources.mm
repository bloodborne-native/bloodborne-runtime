// SPDX-License-Identifier: GPL-2.0-or-later
// The render adapter's Metal resources (M1); see mtl_resources.h.

#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#include "video_core/renderer_metal/mtl_resources.h"

#import <Metal/Metal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <set>

#include "common/assert.h"
#include "video_core/renderer_backend/device.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_metal/mtl_vk_layer.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include <vulkan/vulkan_metal.h>

namespace MetalResources {

namespace {
id<MTLDevice> mtl_device = nil;
} // namespace

bool Enabled() {
    static const bool enabled =
        VideoCore::Backend::SelectedApi() == VideoCore::Backend::Api::Metal;
    return enabled;
}

void Init(void* device) {
    if (!Enabled() || mtl_device) {
        return;
    }
#if BB_METAL_BACKEND
    if (MetalLayer::NativeBoot()) {
        mtl_device = (__bridge id<MTLDevice>)MetalLayer::MtlDevice();
        ASSERT_MSG(mtl_device, "Metal backend has no MTLDevice");
        std::printf("Render adapter: buffers made on the native Metal device %s\n",
                    mtl_device.name.UTF8String);
        return;
    }
#endif
    const VkDevice vk_device = static_cast<VkDevice>(device);
    const auto export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(vk_device, "vkExportMetalObjectsEXT"));
    ASSERT_MSG(export_objects, "BB_GFX_BACKEND=metal needs VK_EXT_metal_objects");
    VkExportMetalDeviceInfoEXT device_info{VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
    VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &device_info};
    export_objects(vk_device, &info);
    mtl_device = device_info.mtlDevice;
    ASSERT_MSG(mtl_device, "MoltenVK gave no Metal device");
    std::printf("Render adapter: buffers made in Metal on %s (BB_GFX_BACKEND=metal)\n",
                mtl_device.name.UTF8String);
}

Buffer NewBuffer(u64 size, Storage storage) {
    // Metal's own hazard tracking, as MoltenVK's buffers have.
    MTLResourceOptions options = MTLResourceHazardTrackingModeDefault;
    switch (storage) {
    case Storage::Private:
        options |= MTLResourceStorageModePrivate;
        break;
    case Storage::SharedWriteCombined:
        options |= MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined;
        break;
    case Storage::Shared:
        options |= MTLResourceStorageModeShared;
        break;
    }
    id<MTLBuffer> buffer = [mtl_device newBufferWithLength:size options:options];
#if TARGET_OS_IPHONE
    // On iOS the app's memory limit is the usual cause: say how much of it was left.
    ASSERT_MSG(buffer, "Metal could not make a buffer of {} bytes ({} MiB left under the app's memory limit)",
               size, os_proc_available_memory() >> 20);
#else
    ASSERT_MSG(buffer, "Metal could not make a buffer of {} bytes", size);
#endif
    return {(__bridge_retained void*)buffer,
            storage == Storage::Private ? nullptr : buffer.contents};
}

Buffer WrapHostMemory(void* host, u64 size) {
    // What MoltenVK does for VK_EXT_external_memory_host: shared storage, no copy, no deallocator.
    id<MTLBuffer> buffer = [mtl_device newBufferWithBytesNoCopy:host
                                                         length:size
                                                        options:MTLResourceStorageModeShared |
                                                                MTLResourceHazardTrackingModeDefault
                                                    deallocator:nil];
    ASSERT_MSG(buffer, "Metal could not use {} bytes of host memory in place", size);
    return {(__bridge_retained void*)buffer, host};
}

std::atomic<u64> live_textures{0}, live_texture_bytes{0};

Texture NewTexture(const TextureDesc& desc) {
    namespace VkToMtl = Metal::VkToMtl;
    const auto format = static_cast<vk::Format>(desc.vk_format);
    const u32 pixel_format = VkToMtl::PixelFormat(format);
    if (pixel_format == 0 || !VkToMtl::IsExactPixelFormat(format) ||
        !(VkToMtl::FormatSwizzle(format) == VkToMtl::IdentitySwizzle)) {
        // Made by MoltenVK instead; each format named once.
        static std::mutex mutex;
        static std::set<u32> named;
        std::scoped_lock lock{mutex};
        if (named.insert(desc.vk_format).second) {
            std::printf("Render adapter: images of format %u made by MoltenVK (no exact Metal "
                        "format)\n",
                        desc.vk_format);
        }
        return {};
    }
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    const bool array = desc.layers > 1;
    // 1D images are 2D textures one texel high, as MoltenVK makes them (its texture1DAs2D
    // default): its views of them are 2D.
    switch (desc.type) {
    case 3:
        td.textureType = MTLTextureType3D;
        break;
    default:
        td.textureType = desc.samples > 1
                             ? (array ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample)
                             : (array ? MTLTextureType2DArray : MTLTextureType2D);
        break;
    }
    td.pixelFormat = static_cast<MTLPixelFormat>(pixel_format);
    td.width = desc.width;
    td.height = desc.height;
    td.depth = desc.depth;
    td.mipmapLevelCount = desc.levels;
    td.arrayLength = desc.layers;
    td.sampleCount = desc.samples;
    td.storageMode = MTLStorageModePrivate;
    MTLTextureUsage usage = MTLTextureUsageUnknown;
    if (desc.shader_read) {
        usage |= MTLTextureUsageShaderRead;
    }
    if (desc.shader_write) {
        usage |= MTLTextureUsageShaderWrite;
    }
    if (desc.render_target) {
        usage |= MTLTextureUsageRenderTarget;
    }
    if (desc.format_views) {
        usage |= MTLTextureUsagePixelFormatView;
    }
    td.usage = usage;
    id<MTLTexture> texture = [mtl_device newTextureWithDescriptor:td];
    if (!texture) {
        return {};
    }
    static std::once_flag first;
    std::call_once(first, [] { std::printf("Render adapter: images made in Metal\n"); });
    live_textures.fetch_add(1, std::memory_order_relaxed);
    live_texture_bytes.fetch_add(texture.allocatedSize, std::memory_order_relaxed);
    return {(__bridge_retained void*)texture, u64(texture.allocatedSize)};
}

void Release(void* object) {
    if (object) {
        id<MTLResource> resource = (__bridge_transfer id<MTLResource>)object;
        if ([resource conformsToProtocol:@protocol(MTLTexture)]) {
            live_textures.fetch_sub(1, std::memory_order_relaxed);
            live_texture_bytes.fetch_sub(resource.allocatedSize, std::memory_order_relaxed);
        }
        resource = nil;
    }
}

void LiveTextures(u64* count, u64* bytes) {
    *count = live_textures.load(std::memory_order_relaxed);
    *bytes = live_texture_bytes.load(std::memory_order_relaxed);
}

} // namespace MetalResources
