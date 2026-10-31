// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <bit>
#include <cstdlib>
#include <mutex>
#include <set>
#include <cstdio>
#include <string>

#include "bbport_guest_memory.h"
#include "bbport_metal_residency.h"
#include "video_core/renderer_metal/mtl_resources.h"
#if BB_METAL_BACKEND
#include "video_core/renderer_metal/mtl_recorder.h"
#include "video_core/renderer_metal/mtl_vk_layer.h"
#endif
#include "video_core/renderer_vulkan/vk_backend_device.h"
#include "video_core/renderer_vulkan/vk_backend_readback.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_metal_shadow.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_resource_backend.h"
#include "video_core/renderer_vulkan/vk_state.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

namespace {
constexpr std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};
} // namespace

BackendDevice::BackendDevice(Frontend::WindowSDL& window, s32 physical_device_index,
                             bool enable_validation, bool enable_crash_diagnostic)
    : instance{window, physical_device_index, enable_validation, enable_crash_diagnostic},
      scheduler{instance, true}, capabilities{QueryResourceCapabilities(instance)},
      descriptor_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
#if BB_METAL_BACKEND
    if (MetalLayer::FrameEnabled()) {
        scheduler.SetEncoder(Metal::FrameStreamEncoder());
        SetActiveStreamEncoder(Metal::FrameStreamEncoder());
        recorder = Metal::MakeFrameRecorder(scheduler, descriptor_heap, instance.GetDevice());
    }
#endif
    if (!recorder) {
        recorder = std::make_unique<CommandRecorder>(scheduler, descriptor_heap, instance.GetDevice());
    }
    instance.SetBackendDevice(this);
    MetalResources::Init(static_cast<VkDevice>(instance.GetDevice()));
    capabilities.non_coherent_atom_size = instance.NonCoherentAtomSize();
    const auto families = instance.GetPhysicalDevice().getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    capabilities.timestamp_valid_bits =
        family < families.size() ? families[family].timestampValidBits : 0u;
    capabilities.timestamp_period_ns =
        instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    capabilities.uniform_min_alignment = instance.UniformMinAlignment();
    capabilities.storage_min_alignment = instance.StorageMinAlignment();
#if BB_METAL_BACKEND
    // bbport: Metal binds a buffer at any dword offset. Without the shaders' offset table a vertex
    // shader no longer depends on how many buffers its pixel shader uses.
    if (MetalLayer::FrameEnabled()) {
        capabilities.storage_min_alignment = 4;
        capabilities.stage_bindings = true;
    }
#endif
    capabilities.integrated = instance.IsIntegrated();
    capabilities.memory_report = instance.CanReportMemoryUsage();
    capabilities.tessellation = instance.IsTessellationSupported();
    capabilities.geometry_shaders = instance.IsGeometryStageSupported();
    capabilities.list_restart = instance.IsListRestartSupported();
    capabilities.patch_list_restart = instance.IsPatchListRestartSupported();
    capabilities.null_descriptor = instance.IsNullDescriptorSupported();
    capabilities.mixed_depth_samples = instance.IsMixedDepthSamplesSupported();
    capabilities.mixed_any_samples = instance.IsMixedAnySamplesSupported();
    capabilities.depth_range_unrestricted = instance.IsDepthRangeUnrestrictedSupported();
    capabilities.depth_clip_control = instance.IsDepthClipControlSupported();
    capabilities.buffer_markers = instance.IsBufferMarkerSupported();
    capabilities.max_viewport_width = instance.GetMaxViewportWidth();
    capabilities.max_viewport_height = instance.GetMaxViewportHeight();
    capabilities.max_framebuffer_width = instance.GetMaxFramebufferWidth();
    capabilities.max_framebuffer_height = instance.GetMaxFramebufferHeight();
    capabilities.max_sampler_allocations = instance.GetMaxSamplerAllocationCount();
    const auto limits = instance.GetPhysicalDevice().getProperties().limits;
    for (u32 i = 0; i < 3; ++i) {
        capabilities.max_compute_groups[i] = limits.maxComputeWorkGroupCount[i];
    }
    capabilities.precise_occlusion = instance.IsOcclusionQueryPrecise();
    capabilities.vertex_stores =
        instance.GetPhysicalDevice().getFeatures().vertexPipelineStoresAndAtomics;
    capabilities.block_texel_views = instance.IsBlockTexelViewSupported();
    VideoCore::Backend::SelectedApi();
    GpuProfiler::Init(instance, scheduler);
}

void BackendDevice::InstallGuestMemory() {
    BbGuestMemory::Install(instance);
}

namespace Backend = VideoCore::Backend;

namespace {
/// Every use a buffer of guest memory has (imported host memory).
constexpr vk::BufferUsageFlags ReadFlags =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;
constexpr vk::BufferUsageFlags AllFlags =
    ReadFlags | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;
} // namespace

Backend::Device::WaitResult BackendDevice::WaitFor(u64 tick, u64 timeout_ns) {
#if BB_METAL_BACKEND
    if (void* event = scheduler.GetWorkSemaphore()->Event()) {
        return MetalLayer::WaitEvent(event, tick, timeout_ns) ? WaitResult::Complete
                                                              : WaitResult::Timeout;
    }
#endif
    const vk::Semaphore handle = scheduler.GetWorkSemaphore()->Handle();
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &handle,
        .pValues = &tick,
    };
    switch (instance.GetDevice().waitSemaphores(&wait_info, timeout_ns)) {
    case vk::Result::eSuccess:
        return WaitResult::Complete;
    case vk::Result::eErrorDeviceLost:
        return WaitResult::DeviceLost;
    default:
        return WaitResult::Timeout;
    }
}

Backend::BufferAllocation BackendDevice::Insert(UniqueBuffer&& buffer, bool sparse) {
    Backend::BufferAllocation result{
        .native = reinterpret_cast<u64>(static_cast<VkBuffer>(buffer.buffer)),
        .device_address = buffer.bda_addr,
        .coherent = !sparse, // imports are coherent; sparse buffers have no host mapping
    };
    if (buffer.allocation && !sparse) {
        VmaAllocationInfo alloc_info{};
        vmaGetAllocationInfo(buffer.allocator, buffer.allocation, &alloc_info);
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(buffer.allocator, buffer.allocation, &property_flags);
        if (alloc_info.pMappedData) {
            result.mapped = std::span<u8>{static_cast<u8*>(alloc_info.pMappedData),
                                          static_cast<size_t>(alloc_info.size)};
        }
        result.coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    if (buffer.metal_contents) {
        // A Metal buffer in shared storage: mapped, coherent (Apple's unified memory).
        result.mapped = std::span<u8>{static_cast<u8*>(buffer.metal_contents),
                                      static_cast<size_t>(buffer.metal_size)};
    }
#if BB_METAL_BACKEND
    // With the Metal frame, binding the buffer then asks MoltenVK nothing (M4).
    if (!sparse) {
        MetalLayer::BufferMade(reinterpret_cast<void*>(result.native));
    }
#endif
    std::scoped_lock lock{objects_mutex};
    result.handle = buffers.Insert(std::move(buffer));
    return result;
}

Backend::BufferAllocation BackendDevice::CreateBuffer(const Backend::BufferDesc& desc,
                                                      std::string_view name) {
    UniqueBuffer buffer{instance.GetDevice(), instance.GetAllocator()};
    CreateBufferFromDesc(instance, buffer, desc, nullptr);
    SetObjectName(instance.GetDevice(), buffer.buffer, name);
    auto result = Insert(std::move(buffer), desc.memory == Backend::MemoryClass::Sparse);
    if (!result.mapped.empty()) {
        result.mapped = result.mapped.first(desc.size_bytes);
    }
    return result;
}

Backend::BufferAllocation BackendDevice::ImportHostMemory(void* host, u64 size,
                                                          std::string_view name) {
    const auto device = instance.GetDevice();
    UniqueBuffer buffer{device, instance.GetAllocator()};
#if BB_METAL_BACKEND
    if (MetalLayer::FrameEnabled()) {
        // The guest memory used in place by Metal alone (M4): no MoltenVK buffer or import.
        const auto metal = MetalResources::WrapHostMemory(host, size);
        buffer.metal_buffer = metal.object;
        buffer.metal_contents = host;
        buffer.metal_size = size;
        buffer.metal_only = true;
        buffer.buffer = vk::Buffer{static_cast<VkBuffer>(MetalLayer::MetalOnlyBuffer(metal.object))};
        buffer.bda_addr = MetalLayer::MetalOnlyBufferAddress(static_cast<VkBuffer>(buffer.buffer));
        BbMetalResidency::AddMetalBuffer(static_cast<VkBuffer>(buffer.buffer), metal.object);
        std::printf("GPU: %s (%llu MiB) used in place by Metal, kept resident\n",
                    std::string{name}.c_str(), static_cast<unsigned long long>(size >> 20));
        auto result = Insert(std::move(buffer), false);
        result.mapped = std::span<u8>{static_cast<u8*>(host), size};
        return result;
    }
#endif
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
    };
    const vk::BufferCreateInfo buffer_ci = {
        .pNext = &external,
        .size = size,
        .usage = AllFlags | vk::BufferUsageFlagBits::eUniformTexelBuffer |
                 vk::BufferUsageFlagBits::eStorageTexelBuffer,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    buffer.buffer = Check(device.createBuffer(buffer_ci));
    const auto [props_result, props] = device.getMemoryHostPointerPropertiesEXT(
        vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, host);
    ASSERT_MSG(props_result == vk::Result::eSuccess && props.memoryTypeBits != 0,
               "Host memory cannot be imported: {}", vk::to_string(props_result));
    const auto requirements = device.getBufferMemoryRequirements(buffer.buffer);
    const u32 types = props.memoryTypeBits & requirements.memoryTypeBits;
    ASSERT_MSG(types != 0, "No memory type for imported host memory");
    const vk::ImportMemoryHostPointerInfoEXT import_info{
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
        .pHostPointer = host,
    };
    const vk::MemoryAllocateFlagsInfo flags_info{
        .pNext = &import_info,
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const auto [memory_result, memory] = device.allocateMemory({
        .pNext = &flags_info,
        .allocationSize = size,
        .memoryTypeIndex = static_cast<u32>(std::countr_zero(types)),
    });
    ASSERT_MSG(memory_result == vk::Result::eSuccess, "Importing host memory failed: {}",
               vk::to_string(memory_result));
    const auto bind_result = device.bindBufferMemory(buffer.buffer, memory, 0);
    ASSERT_MSG(bind_result == vk::Result::eSuccess, "Binding imported host memory failed: {}",
               vk::to_string(bind_result));
    buffer.bda_addr = device.getBufferAddress({.buffer = buffer.buffer});
    ASSERT_MSG(buffer.bda_addr != 0, "Failed to get buffer device address");
    SetObjectName(device, buffer.buffer, name);
    // bbport: wired once instead of for every command buffer that uses it (BB_RESIDENCY_SET).
    if (BbMetalResidency::AddMemory(memory)) {
        std::printf("GPU: %s (%llu MiB) kept resident (Metal residency set)\n",
                    std::string{name}.c_str(), static_cast<unsigned long long>(size >> 20));
    }
    // The memory lives for the process: it is not freed with the buffer.
    auto result = Insert(std::move(buffer), false);
    result.mapped = std::span<u8>{static_cast<u8*>(host), size};
    return result;
}

Backend::BufferAllocation BackendDevice::ImportGuestMemoryChunk(u64 native_memory, u64 size,
                                                                std::string_view name) {
    const auto device = instance.GetDevice();
    UniqueBuffer buffer{device, instance.GetAllocator()};
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = BbGuestMemory::HandleType(),
    };
    // bbport BB_LAYER_MEMORY: what the GPU binds for the game's memory in place (no arena):
    // vertex, index and indirect data and device addresses too (the memory has them).
    const bool layer = BbGuestMemory::LayerMemory();
    vk::BufferUsageFlags usage =
        vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
        vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
        vk::BufferUsageFlagBits::eUniformTexelBuffer | vk::BufferUsageFlagBits::eStorageTexelBuffer;
    if (layer) {
        usage |= vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
                 vk::BufferUsageFlagBits::eIndirectBuffer |
                 vk::BufferUsageFlagBits::eShaderDeviceAddress;
    }
    const vk::BufferCreateInfo buffer_ci = {
        .pNext = &external,
        .size = size,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    buffer.buffer = Check(device.createBuffer(buffer_ci));
    const vk::DeviceMemory memory{reinterpret_cast<VkDeviceMemory>(native_memory)};
    const auto result = device.bindBufferMemory(buffer.buffer, memory, 0);
    ASSERT_MSG(result == vk::Result::eSuccess, "Binding a guest memory chunk failed: {}",
               vk::to_string(result));
    if (layer) {
        buffer.bda_addr = device.getBufferAddress({.buffer = buffer.buffer});
    }
    SetObjectName(device, buffer.buffer, name);
    return Insert(std::move(buffer), false);
}

void BackendDevice::DestroyBuffer(Backend::BufferHandle buffer) {
    if (!buffer) {
        return;
    }
    UniqueBuffer object{instance.GetDevice(), instance.GetAllocator()};
    {
        std::scoped_lock lock{objects_mutex};
        UniqueBuffer* entry = buffers.Get(buffer);
        if (!entry) {
            return;
        }
        object = std::move(*entry);
        buffers.Erase(buffer);
    }
    DeferRelease([object = std::move(object)] {
#if BB_METAL_BACKEND
        MetalLayer::BufferGone(static_cast<VkBuffer>(object.buffer));
#endif
    });
}

void BackendDevice::FlushBuffer(Backend::BufferHandle buffer, u64 offset, u64 size) {
    std::scoped_lock lock{objects_mutex};
    if (const UniqueBuffer* entry = buffers.Get(buffer); entry && entry->allocation) {
        vmaFlushAllocation(entry->allocator, entry->allocation, offset, size);
    }
}

void BackendDevice::InvalidateBuffer(Backend::BufferHandle buffer, u64 offset, u64 size) {
    std::scoped_lock lock{objects_mutex};
    if (const UniqueBuffer* entry = buffers.Get(buffer); entry && entry->allocation) {
        vmaInvalidateAllocation(entry->allocator, entry->allocation, offset, size);
    }
}

u64 BackendDevice::NativeHandle(Backend::BufferHandle buffer) {
    std::scoped_lock lock{objects_mutex};
    const UniqueBuffer* entry = buffers.Get(buffer);
    return entry ? reinterpret_cast<u64>(static_cast<VkBuffer>(entry->buffer)) : 0;
}

vk::ImageCreateInfo BackendDevice::ImageCreateInfo(const Backend::ImageDesc& desc) const {
    vk::ImageCreateFlags flags{};
    if (desc.mutable_format) {
        flags |= vk::ImageCreateFlagBits::eMutableFormat | vk::ImageCreateFlagBits::eExtendedUsage;
    }
    if (desc.views_of_3d_slices) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (desc.uncompressed_block_views && desc.mutable_format &&
        instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }
    return vk::ImageCreateInfo{
        .flags = flags,
        .imageType = ToVk(desc.type),
        .format = ToVk(desc.format),
        .extent{.width = desc.width, .height = desc.height, .depth = desc.depth},
        .mipLevels = desc.levels,
        .arrayLayers = desc.layers,
        .samples = static_cast<vk::SampleCountFlagBits>(desc.samples),
        .tiling = vk::ImageTiling::eOptimal,
        .usage = ToVk(desc.usage),
        .initialLayout = vk::ImageLayout::eUndefined,
    };
}

Backend::Format BackendDevice::SupportedFormat(Backend::Format format, Backend::ImageUse usage) {
    return FromVk(instance.GetSupportedFormat(ToVk(format), FormatFeatureFlags(ToVk(usage))));
}

bool BackendDevice::CanBlit(Backend::Format format) {
    // Asked for every attachment of every draw (scene resolution): the driver query was ~5% of
    // the GPU thread, so answers are kept.
    const u32 index = static_cast<u32>(format);
    if (index < blit_table.size() && blit_table[index]) {
        return blit_table[index] == 2;
    }
    if (index >= blit_table.size()) {
        if (const auto it = blit_formats.find(index); it != blit_formats.end()) {
            return it->second;
        }
    }
    const auto features =
        instance.GetPhysicalDevice().getFormatProperties(ToVk(format)).optimalTilingFeatures;
    const auto required = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
    const bool blittable = (features & required) == required;
    if (index < blit_table.size()) {
        blit_table[index] = blittable ? 2 : 1;
    } else {
        blit_formats.emplace(index, blittable);
    }
    return blittable;
}

u32 BackendDevice::SupportedSampleCounts(const Backend::ImageDesc& desc) {
    const vk::ImageCreateInfo ci = ImageCreateInfo(desc);
    const auto key = std::tuple{u32(ci.format), u32(ci.imageType), u32(ci.usage), u32(ci.flags)};
    {
        std::scoped_lock lock{sample_counts_mutex};
        if (const auto it = sample_counts.find(key); it != sample_counts.end()) {
            return it->second;
        }
    }
    const vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = ci.format,
        .type = ci.imageType,
        .tiling = ci.tiling,
        .usage = ci.usage,
        .flags = ci.flags,
    };
    const auto properties = instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    if (properties.result == vk::Result::eErrorFormatNotSupported) {
        // bbport: once per format and type. MoltenVK reports block-compressed formats with storage
        // usage as unsupported yet creates the images; one line per texture flooded the log.
        static std::mutex reported_mutex;
        static std::set<std::pair<vk::Format, vk::ImageType>> reported;
        std::scoped_lock lk{reported_mutex};
        if (reported.emplace(format_info.format, format_info.type).second) {
            LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                      vk::to_string(format_info.format), vk::to_string(format_info.type),
                      vk::to_string(format_info.flags), vk::to_string(format_info.usage));
        }
    }
    const u32 counts = properties.result == vk::Result::eSuccess
                           ? static_cast<u32>(properties.value.imageFormatProperties.sampleCounts)
                           : 1u;
    std::scoped_lock lock{sample_counts_mutex};
    sample_counts.emplace(key, counts);
    return counts;
}

Backend::ImageAllocation BackendDevice::CreateImage(const Backend::ImageDesc& desc,
                                                    std::string_view name) {
    auto record = std::make_unique<ImageRecord>();
    record->image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    if (const auto create_info = ImageCreateInfo(desc); !record->image.CreateInMetal(create_info)) {
#if BB_METAL_BACKEND
        ASSERT_MSG(!MetalLayer::FrameEnabled(),
                   "Metal frame image allocation fell back to VMA (format {}, flags {}, usage {})",
                   vk::to_string(create_info.format), vk::to_string(create_info.flags),
                   vk::to_string(create_info.usage));
#endif
        record->image.Create(create_info);
    }
    SetObjectName(instance.GetDevice(), record->image.image, name);
#if BB_METAL_BACKEND
    // A MoltenVK image: the Metal frame exports its texture once, not on every use (M4).
    if (!record->image.metal_only) {
        MetalLayer::ImageMade(static_cast<VkImage>(record->image.image));
    }
#endif
    Backend::ImageAllocation result{
        .native = reinterpret_cast<u64>(static_cast<VkImage>(record->image.image)),
        .size_bytes = record->image.size_bytes,
        .backend_state = &record->state,
    };
    std::scoped_lock lock{objects_mutex};
    result.handle = images.Insert(std::move(record));
    return result;
}

void BackendDevice::DestroyImage(Backend::ImageHandle image) {
    if (!image) {
        return;
    }
    std::unique_ptr<ImageRecord> object;
    {
        std::scoped_lock lock{objects_mutex};
        std::unique_ptr<ImageRecord>* entry = images.Get(image);
        if (!entry) {
            return;
        }
        object = std::move(*entry);
        images.Erase(image);
    }
    DeferRelease([object = std::move(object)] {
#if BB_METAL_BACKEND
        if (!object->image.metal_only) {
            MetalLayer::ImageGone(static_cast<VkImage>(object->image.image));
        }
#endif
    });
}

u64 BackendDevice::NativeHandle(Backend::ImageHandle image) {
    std::scoped_lock lock{objects_mutex};
    const std::unique_ptr<ImageRecord>* entry = images.Get(image);
    return entry ? reinterpret_cast<u64>(static_cast<VkImage>((*entry)->image.image)) : 0;
}

Backend::ImageViewAllocation BackendDevice::CreateImageView(Backend::ImageHandle image,
                                                            const Backend::ImageViewDesc& desc,
                                                            std::string_view name) {
    vk::Image native_image{};
    vk::Format image_format{};
    {
        std::scoped_lock lock{objects_mutex};
        const std::unique_ptr<ImageRecord>* entry = images.Get(image);
        ASSERT_MSG(entry, "Creating a view of a destroyed image");
        native_image = (*entry)->image.image;
        image_format = (*entry)->image.image_ci.format;
    }
    // Metal cannot view a block-compressed texture as an uncompressed format (M4): each kind of
    // such view is logged once, so it shows whether the game makes any.
    const auto compressed = [](vk::Format f) {
        return f >= vk::Format::eBc1RgbUnormBlock && f <= vk::Format::eAstc12x12SrgbBlock;
    };
    if (compressed(image_format) && !compressed(ToVk(desc.format))) {
        static std::mutex logged_mutex;
        static std::set<std::pair<vk::Format, vk::Format>> logged;
        std::scoped_lock lock{logged_mutex};
        if (logged.emplace(image_format, ToVk(desc.format)).second) {
            std::printf("Image views: uncompressed view %s of a %s image\n",
                        vk::to_string(ToVk(desc.format)).c_str(),
                        vk::to_string(image_format).c_str());
        }
    }
    vk::ImageViewUsageCreateInfo usage_ci{.usage = ToVk(desc.usage)};
    vk::ImageViewMinLodCreateInfoEXT min_lod_ci{.minLod = desc.min_lod};
    if (desc.min_lod != 0.0f && instance.IsImageViewMinLodSupported()) {
        usage_ci.pNext = &min_lod_ci;
    }
    const vk::ImageViewCreateInfo image_view_ci = {
        .pNext = &usage_ci,
        .image = native_image,
        .viewType = ToVk(desc.type),
        .format = ToVk(desc.format),
        .components = ToVk(desc.mapping),
        .subresourceRange{
            .aspectMask = ToVk(desc.aspect),
            .baseMipLevel = desc.base_level,
            .levelCount = desc.levels,
            .baseArrayLayer = desc.base_layer,
            .layerCount = desc.layers,
        },
    };
    auto [view_result, view] = instance.GetDevice().createImageViewUnique(image_view_ci);
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create image view: {}",
               vk::to_string(view_result));
    SetObjectName(instance.GetDevice(), *view, name);
    Backend::ImageViewAllocation result{
        .native = reinterpret_cast<u64>(static_cast<VkImageView>(*view)),
    };
    std::scoped_lock lock{objects_mutex};
    result.handle = views.Insert(std::move(view));
    return result;
}

void BackendDevice::DestroyImageView(Backend::ImageViewHandle view) {
    if (!view) {
        return;
    }
    vk::UniqueImageView object;
    {
        std::scoped_lock lock{objects_mutex};
        vk::UniqueImageView* entry = views.Get(view);
        if (!entry) {
            return;
        }
        object = std::move(*entry);
        views.Erase(view);
    }
    DeferRelease([object = std::move(object)] {});
}

u64 BackendDevice::NativeHandle(Backend::ImageViewHandle view) {
    std::scoped_lock lock{objects_mutex};
    const vk::UniqueImageView* entry = views.Get(view);
    return entry ? reinterpret_cast<u64>(static_cast<VkImageView>(**entry)) : 0;
}

Backend::ShaderAllocation BackendDevice::CreateShader(std::span<const u32> spirv,
                                                      std::string_view name) {
#if BB_METAL_BACKEND
    if (MetalLayer::FrameEnabled()) {
        // Only Metal builds pipelines (MetalOnlyPipeline): the shader is its SPIR-V, registered
        // under a placeholder handle MoltenVK never sees, and no MoltenVK module holds a copy.
        static std::atomic<u64> next_placeholder{1};
        static std::atomic<u64> spirv_bytes{0};
        const u64 count = next_placeholder++;
        const u64 native = 0xBC00000000000000ull | count;
        const u64 bytes = spirv_bytes += spirv.size_bytes();
        if (count % 200 == 0) {
            std::printf("Metal frame: %llu shaders made, %.1f MiB of SPIR-V (no MoltenVK module "
                        "copies)\n",
                        static_cast<unsigned long long>(count), double(bytes) / (1 << 20));
        }
        MetalShadow::RegisterModule(vk::ShaderModule{reinterpret_cast<VkShaderModule>(native)},
                                    spirv);
        Backend::ShaderAllocation allocation{.native = native};
        std::scoped_lock lock{objects_mutex};
        allocation.handle = shaders.Insert(ShaderRecord{.native = native});
        return allocation;
    }
#endif
    auto [result, module] = instance.GetDevice().createShaderModuleUnique(
        {.codeSize = spirv.size_bytes(), .pCode = spirv.data()});
    if (result != vk::Result::eSuccess) {
        return {};
    }
    // The Metal path builds its pipeline states from the same SPIR-V.
    if (MetalShadow::Enabled()) {
        MetalShadow::RegisterModule(*module, spirv);
    }
    if (!name.empty()) {
        SetObjectName(instance.GetDevice(), *module, name);
    }
    const u64 native = reinterpret_cast<u64>(static_cast<VkShaderModule>(*module));
    Backend::ShaderAllocation allocation{.native = native};
    std::scoped_lock lock{objects_mutex};
    allocation.handle = shaders.Insert(ShaderRecord{.module = std::move(module), .native = native});
    return allocation;
}

void BackendDevice::DestroyShader(Backend::ShaderHandle shader) {
    if (!shader) {
        return;
    }
    vk::UniqueShaderModule object;
    {
        std::scoped_lock lock{objects_mutex};
        object = ReleaseShaderLocked(shader);
    }
    if (object) {
        DeferRelease([object = std::move(object)] {});
    }
}

vk::UniqueShaderModule BackendDevice::ReleaseShaderLocked(Backend::ShaderHandle shader) {
    ShaderRecord* entry = shaders.Get(shader);
    if (!entry || --entry->holders != 0) {
        return {};
    }
    vk::UniqueShaderModule object = std::move(entry->module);
    if (!object && MetalShadow::Enabled()) {
        // The Metal frame's placeholder: its SPIR-V goes (pipelines made from it hold their
        // compiled forms).
        MetalShadow::ForgetModule(entry->Module());
    }
    shaders.Erase(shader);
    return object;
}

u64 BackendDevice::NativeHandle(Backend::ShaderHandle shader) {
    std::scoped_lock lock{objects_mutex};
    const ShaderRecord* entry = shaders.Get(shader);
    return entry ? entry->native : 0;
}

Backend::QueryPoolAllocation BackendDevice::CreateOcclusionQueries(u32 count,
                                                                  std::string_view name) {
    return CreateQueries(vk::QueryType::eOcclusion, count, name);
}

Backend::QueryPoolAllocation BackendDevice::CreateTimestampQueries(u32 count,
                                                                  std::string_view name) {
    return CreateQueries(vk::QueryType::eTimestamp, count, name);
}

vk::QueryPool BackendDevice::NativeQueryPool(Backend::QueryPoolHandle pool) {
    std::scoped_lock lock{objects_mutex};
    const vk::UniqueQueryPool* entry = query_pools.Get(pool);
    return entry ? **entry : vk::QueryPool{};
}

void BackendDevice::ResetQueries(Backend::QueryPoolHandle pool, u32 first, u32 count) {
    instance.GetDevice().resetQueryPool(NativeQueryPool(pool), first, count);
}

bool BackendDevice::ReadQueryResults(Backend::QueryPoolHandle pool, u32 first,
                                     std::span<u64> values, bool wait) {
    const auto flags = vk::QueryResultFlagBits::e64 |
                       (wait ? vk::QueryResultFlagBits::eWait : vk::QueryResultFlags{});
    return instance.GetDevice().getQueryPoolResults(
               NativeQueryPool(pool), first, static_cast<u32>(values.size()), values.size_bytes(),
               values.data(), sizeof(u64), flags) == vk::Result::eSuccess;
}

Backend::QueryPoolAllocation BackendDevice::CreateQueries(vk::QueryType type, u32 count,
                                                         std::string_view name) {
    auto pool = Check(instance.GetDevice().createQueryPoolUnique({
        .queryType = type,
        .queryCount = count,
    }));
    if (!name.empty()) {
        SetObjectName(instance.GetDevice(), *pool, name);
    }
    Backend::QueryPoolAllocation allocation{
        .native = reinterpret_cast<u64>(static_cast<VkQueryPool>(*pool)),
    };
    std::scoped_lock lock{objects_mutex};
    allocation.handle = query_pools.Insert(std::move(pool));
    return allocation;
}

void BackendDevice::DestroyQueryPool(Backend::QueryPoolHandle pool) {
    if (!pool) {
        return;
    }
    vk::UniqueQueryPool object;
    {
        std::scoped_lock lock{objects_mutex};
        vk::UniqueQueryPool* entry = query_pools.Get(pool);
        if (!entry) {
            return;
        }
        object = std::move(*entry);
        query_pools.Erase(pool);
    }
    DeferRelease([object = std::move(object)] {});
}

Backend::SamplerHandle BackendDevice::CreateSampler(const Backend::SamplerDesc& desc) {
    const bool custom = desc.border_color == Backend::BorderColor::FloatCustom;
    const vk::SamplerCustomBorderColorCreateInfoEXT custom_color{
        .customBorderColor = vk::ClearColorValue{.float32 = desc.border_color_value},
        .format = vk::Format::eR32G32B32A32Sfloat,
    };
    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom ? &custom_color : nullptr,
        .magFilter = ToVk(desc.mag_filter),
        .minFilter = ToVk(desc.min_filter),
        .mipmapMode = ToVk(desc.mipmap_mode),
        .addressModeU = ToVk(desc.address_u),
        .addressModeV = ToVk(desc.address_v),
        .addressModeW = ToVk(desc.address_w),
        .mipLodBias = desc.lod_bias,
        .anisotropyEnable = desc.max_anisotropy > 1.0f,
        .maxAnisotropy = desc.max_anisotropy,
        .compareEnable = desc.compare_enable,
        .compareOp = ToVk(desc.compare_op),
        .minLod = desc.min_lod,
        .maxLod = desc.max_lod,
        .borderColor = ToVk(desc.border_color),
        .unnormalizedCoordinates = false,
    };
    auto [result, sampler] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(result));
    std::scoped_lock lock{objects_mutex};
    return samplers.Insert(std::move(sampler));
}

void BackendDevice::DestroySampler(Backend::SamplerHandle sampler) {
    if (!sampler) {
        return;
    }
    vk::UniqueSampler object;
    {
        std::scoped_lock lock{objects_mutex};
        vk::UniqueSampler* entry = samplers.Get(sampler);
        if (!entry) {
            return;
        }
        object = std::move(*entry);
        samplers.Erase(sampler);
    }
    DeferRelease([object = std::move(object)] {});
}

u64 BackendDevice::NativeHandle(Backend::SamplerHandle sampler) {
    std::scoped_lock lock{objects_mutex};
    const vk::UniqueSampler* entry = samplers.Get(sampler);
    return entry ? reinterpret_cast<u64>(static_cast<VkSampler>(**entry)) : 0;
}

Shader::Profile BackendDevice::ShaderProfile() const {
    constexpr u32 SpirvVersion1_6 = 0x00010600U;
    const auto& vk12_props = instance.GetVk12Properties();
    return Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // bbport: older NVIDIA (Pascal) has no barycentrics; BB_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("BB_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = GetCapabilities().storage_min_alignment > 4,
        .stage_bindings = GetCapabilities().stage_bindings,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance.IsShaderStencilExportSupported(),
    };
}

Backend::MemoryReport BackendDevice::ReportMemory() const {
    if (!instance.CanReportMemoryUsage()) {
        return {.total = instance.GetTotalMemoryBudget()};
    }
#if BB_METAL_BACKEND
    // With the Metal frame, Metal's own numbers (what MoltenVK reports), without a Vulkan call.
    unsigned long long metal_used = 0, metal_budget = 0;
    if (MetalLayer::MemoryUsage(&metal_used, &metal_budget)) {
        static std::once_flag compared;
        std::call_once(compared, [&] {
            if (MetalLayer::NativeBoot()) {
                std::printf("Memory report: native Metal %llu MiB used of %llu\n",
                            metal_used >> 20, metal_budget >> 20);
            } else {
                const auto [vk_used, vk_budget] = instance.GetDeviceMemoryUsageAndBudget();
                std::printf("Memory report: Metal %llu MiB used of %llu, MoltenVK %llu of %llu\n",
                            metal_used >> 20, metal_budget >> 20,
                            static_cast<unsigned long long>(vk_used >> 20),
                            static_cast<unsigned long long>(vk_budget >> 20));
            }
        });
        const u64 limit = Instance::VramLimitBytes();
        return {
            .available = true,
            .used = metal_used,
            .budget = limit != 0 ? std::min<u64>(metal_budget, limit) : metal_budget,
            .total = instance.GetTotalMemoryBudget(),
        };
    }
#endif
    const auto [used, budget] = instance.GetDeviceMemoryUsageAndBudget();
    return {
        .available = true,
        .used = used,
        .budget = budget,
        .total = instance.GetTotalMemoryBudget(),
    };
}

} // namespace Vulkan
