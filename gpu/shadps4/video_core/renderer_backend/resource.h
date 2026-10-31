// Project-owned resource descriptions shared by Vulkan and future backends.
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <span>

#include "common/types.h"
#include "video_core/renderer_backend/format.h"
#include "video_core/renderer_backend/state.h"

namespace VideoCore::Backend {

// These are identities, not native API handles. The backend will issue them as
// allocation and deferred destruction move behind this boundary; zero is never
// a live generation.
template <typename Tag>
struct Handle {
    u32 slot{};
    u32 generation{};

    explicit operator bool() const noexcept {
        return generation != 0;
    }
    friend bool operator==(Handle, Handle) = default;
};
struct BufferTag;
struct ImageTag;
struct ImageViewTag;
struct SamplerTag;
struct ShaderTag;
struct PipelineTag;
struct QueryPoolTag;
using BufferHandle = Handle<BufferTag>;
using ImageHandle = Handle<ImageTag>;
using ImageViewHandle = Handle<ImageViewTag>;
using SamplerHandle = Handle<SamplerTag>;
using ShaderHandle = Handle<ShaderTag>;
using PipelineHandle = Handle<PipelineTag>;
using QueryPoolHandle = Handle<QueryPoolTag>;

/// Queries the device made: occlusion queries (the samples that pass between
/// CommandRecorder::BeginQuery and EndQuery) or timestamps (CommandRecorder::WriteTimestamp).
struct QueryPoolAllocation {
    QueryPoolHandle handle;
    u64 native{}; ///< the backend's object
};

enum class MemoryClass : u8 {
    DeviceLocal,
    HostUncached, // Host-visible memory optimized for uploads.
    HostCached,   // Host-visible memory optimized for readback.
    Stream,       // Prefer device-local host-visible memory, with a host-memory fallback.
    Sparse,       // Unbacked resource; the backend binds its memory separately.
};

enum class BufferUse : u32 {
    None = 0,
    TransferSource = 1u << 0,
    TransferDestination = 1u << 1,
    Uniform = 1u << 2,
    Storage = 1u << 3,
    Vertex = 1u << 4,
    Index = 1u << 5,
    Indirect = 1u << 6,
    DeviceAddress = 1u << 7,
};

constexpr BufferUse operator|(BufferUse a, BufferUse b) noexcept {
    return static_cast<BufferUse>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool Has(BufferUse uses, BufferUse use) noexcept {
    return (static_cast<u32>(uses) & static_cast<u32>(use)) != 0;
}

struct BufferDesc {
    u64 size_bytes{};
    BufferUse uses{BufferUse::None};
    MemoryClass memory{MemoryClass::DeviceLocal};
    bool concurrent_readback{};
};

/// A buffer the device created or imported.
struct BufferAllocation {
    BufferHandle handle;
    u64 native{};           ///< Device::NativeHandle(handle)
    std::span<u8> mapped{}; ///< host mapping, empty when the memory is not host-visible
    u64 device_address{};   ///< 0 without BufferUse::DeviceAddress
    bool coherent{};        ///< host writes need no Device::FlushBuffer
};

// Image vocabulary, with Vulkan's values (checked in renderer_vulkan/vk_state.h).
enum class ImageType : s32 {
    Image1D = 0,
    Image2D = 1,
    Image3D = 2,
};

enum class ImageUse : u32 {
    None = 0,
    TransferSource = 1u << 0,
    TransferDestination = 1u << 1,
    Sampled = 1u << 2,
    Storage = 1u << 3,
    ColorAttachment = 1u << 4,
    DepthStencilAttachment = 1u << 5,
    AttachmentFeedbackLoop = 1u << 19, ///< read while bound as an attachment
};
constexpr ImageUse operator|(ImageUse a, ImageUse b) noexcept {
    return static_cast<ImageUse>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool Has(ImageUse uses, ImageUse use) noexcept {
    return (static_cast<u32>(uses) & static_cast<u32>(use)) != 0;
}

struct ImageDesc {
    ImageType type{ImageType::Image2D};
    Format format{Format::Undefined};
    u32 width{1}, height{1}, depth{1};
    u32 levels{1};
    u32 layers{1};
    u32 samples{1};
    ImageUse usage{ImageUse::None};
    bool views_of_3d_slices{};       ///< a 3D image viewed as 2D slices
    bool uncompressed_block_views{}; ///< a block-compressed image viewed as uncompressed
    /// Views in other formats of the same size. Metal may leave images that allow them
    /// uncompressed: the upscaler's images cost MetalFX ~2.5% so (docs/PERFORMANCE.md).
    bool mutable_format{true};

    auto operator<=>(const ImageDesc&) const = default;
};

enum class ImageViewType : s32 {
    View1D = 0,
    View2D = 1,
    View3D = 2,
    Cube = 3,
    View1DArray = 4,
    View2DArray = 5,
    CubeArray = 6,
};

enum class ImageAspect : u32 {
    None = 0,
    Color = 1u << 0,
    Depth = 1u << 1,
    Stencil = 1u << 2,
};
constexpr ImageAspect operator|(ImageAspect a, ImageAspect b) noexcept {
    return static_cast<ImageAspect>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr bool Has(ImageAspect aspects, ImageAspect aspect) noexcept {
    return (static_cast<u32>(aspects) & static_cast<u32>(aspect)) != 0;
}

enum class ComponentSwizzle : s32 {
    Identity = 0,
    Zero = 1,
    One = 2,
    R = 3,
    G = 4,
    B = 5,
    A = 6,
};

struct ComponentMapping {
    ComponentSwizzle r{}, g{}, b{}, a{};

    auto operator<=>(const ComponentMapping&) const = default;
};

struct ImageViewDesc {
    ImageViewType type{ImageViewType::View2D};
    Format format{Format::Undefined}; ///< already one the device supports
    ImageAspect aspect{ImageAspect::Color};
    u32 base_level{}, levels{1};
    u32 base_layer{}, layers{1};
    ComponentMapping mapping{};
    ImageUse usage{ImageUse::None}; ///< the uses of the view, a subset of the image's
    float min_lod{};                ///< 0: none
};

struct ImageViewAllocation {
    ImageViewHandle handle;
    u64 native{}; ///< Device::NativeHandle(handle)
};

/// A colour clear's value as 32-bit words, read as float, signed or unsigned per the image's
/// format (as VkClearColorValue).
using ClearColor = std::array<u32, 4>;

/// How a draw or dispatch uses an image; each backend derives its layouts and barriers.
enum class ImageAccess : u8 {
    Sampled,                   ///< read by shaders (depth images: depth read-only)
    Storage,                   ///< read and written by shaders
    SampledWhileTarget,        ///< read by shaders while bound as a color target
    ColorAttachment,           ///< drawn into
    ColorAttachmentFeedback,   ///< drawn into while shaders read it
    DepthAttachment,           ///< depth (and stencil) tested and written
    DepthReadOnlyStencilWrite, ///< depth tested only, stencil written
    DepthReadOnly,             ///< depth (and stencil) tested only
    ComputeRead,               ///< read by compute shaders (the renderer's own passes)
    ComputeReadWrite,          ///< read and written by compute shaders
    TransferSource,            ///< copied from
    TransferDestination,       ///< copied into
};

/// How a draw or dispatch uses a range of a buffer.
enum class BufferAccess : u8 {
    GraphicsRead,
    GraphicsReadWrite,
    ComputeRead,
    ComputeReadWrite,
    TransferWrite,
};

/// A region copied between buffers (bytes).
struct BufferCopy {
    u64 src_offset{};
    u64 dst_offset{};
    u64 size{};
};

/// A region copied between a buffer and an image.
struct BufferImageCopy {
    u64 buffer_offset{};
    u32 buffer_row_length{};   ///< texels per row in the buffer; 0: tightly packed
    u32 buffer_image_height{}; ///< rows per slice in the buffer; 0: tightly packed
    ImageAspect aspect{ImageAspect::Color};
    u32 level{};
    u32 base_layer{};
    u32 layers{1};
    s32 x{}, y{}, z{};
    u32 width{1}, height{1}, depth{1};
};

/// A shader the device made from SPIR-V; an empty handle when the driver rejected the code.
struct ShaderAllocation {
    ShaderHandle handle;
    u64 native{}; ///< Device::NativeHandle(handle)
};

/// An image the device created.
struct ImageAllocation {
    ImageHandle handle;
    u64 native{};     ///< Device::NativeHandle(handle)
    u64 size_bytes{}; ///< memory it occupies
    /// The backend's own record of the image (its barrier state), at a fixed address until the
    /// image is destroyed; only the backend reads it.
    void* backend_state{};
};

struct SamplerDesc {
    Filter mag_filter{Filter::Nearest};
    Filter min_filter{Filter::Nearest};
    MipmapMode mipmap_mode{MipmapMode::Nearest};
    AddressMode address_u{AddressMode::Repeat};
    AddressMode address_v{AddressMode::Repeat};
    AddressMode address_w{AddressMode::Repeat};
    float lod_bias{};
    float min_lod{};
    float max_lod{};
    float max_anisotropy{1.0f}; ///< 1: anisotropic filtering off
    bool compare_enable{};
    CompareOp compare_op{CompareOp::Never};
    BorderColor border_color{BorderColor::FloatTransparentBlack};
    std::array<float, 4> border_color_value{}; ///< for BorderColor::FloatCustom
};

struct Capabilities {
    bool sparse_buffers{};
    bool imported_host_memory{};
    bool exported_guest_memory{};
    bool buffer_device_address{};
    bool custom_border_color{};
    float max_sampler_anisotropy{1.0f}; ///< 1: anisotropic filtering unsupported
    float max_sampler_lod_bias{};
    u64 non_coherent_atom_size{1}; ///< granularity of FlushBuffer and InvalidateBuffer
    bool attachment_feedback_loop{}; ///< ImageUse::AttachmentFeedbackLoop is available
    /// Vertex input is set per draw (CommandRecorder::SetVertexInput), not fixed in pipelines.
    bool dynamic_vertex_input{};
    u32 color_sample_counts{1}; ///< render target sample counts, as a mask (bit n: 2^n samples)
    u32 depth_sample_counts{1};
    u32 timestamp_valid_bits{}; ///< 0: no timestamps
    double timestamp_period_ns{}; ///< nanoseconds per timestamp tick
    u64 uniform_min_alignment{1}; ///< of uniform buffer binding offsets
    u64 storage_min_alignment{1}; ///< of storage buffer binding offsets
    bool stage_bindings{};
    bool integrated{};            ///< the GPU shares the CPU's memory
    bool memory_report{};         ///< Device::ReportMemory has usage and budget (a driver query)
    bool tessellation{};
    bool geometry_shaders{};
    bool list_restart{};       ///< primitive restart in list topologies
    bool patch_list_restart{}; ///< and in patch lists
    bool null_descriptor{};    ///< empty slots may be bound (no null resources needed)
    bool mixed_depth_samples{}; ///< a depth target's sample count may differ from the colours'
    bool mixed_any_samples{};
    bool depth_range_unrestricted{};
    bool depth_clip_control{};
    bool buffer_markers{}; ///< labels written at the end of the pipe inside passes
    u32 max_viewport_width{}, max_viewport_height{};
    u32 max_framebuffer_width{}, max_framebuffer_height{};
    u32 max_sampler_allocations{};
    u32 max_compute_groups[3]{}; ///< workgroups of one dispatch, per dimension
    bool precise_occlusion{};    ///< occlusion queries count samples (else any or none)
    bool vertex_stores{};        ///< vertex shaders may write storage buffers
    bool block_texel_views{};    ///< a block-compressed image may have uncompressed views
};

/// The device's memory as the driver reports it.
struct MemoryReport {
    bool available{}; ///< the driver reports usage and budget
    u64 used{};       ///< by this process, in the heaps the budget counts
    u64 budget{};     ///< the driver's current budget of those heaps
    u64 total{};      ///< the total budget at startup
};

} // namespace VideoCore::Backend

template <typename Tag>
struct std::hash<VideoCore::Backend::Handle<Tag>> {
    std::size_t operator()(VideoCore::Backend::Handle<Tag> handle) const noexcept {
        return std::hash<u64>{}((u64(handle.generation) << 32) | handle.slot);
    }
};
