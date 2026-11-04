// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <ranges>
#include <boost/container/small_vector.hpp>
#include "bbport_toggles.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#if BB_METAL_BACKEND
#include "video_core/renderer_metal/mtl_vk_layer.h"
#endif
#include "video_core/renderer_vulkan/vk_backend_device.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_state.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_depth_resampler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vulkan/vulkan_format_traits.hpp>

namespace Vulkan {

namespace {
/// bbport: a pipeline stage of draws (or every stage).
bool IsGraphicsStage(vk::PipelineStageFlags2 stages) {
    using S = vk::PipelineStageFlagBits2;
    constexpr vk::PipelineStageFlags2 graphics =
        S::eAllCommands | S::eAllGraphics | S::eVertexShader | S::eTessellationControlShader |
        S::eTessellationEvaluationShader | S::eGeometryShader | S::eFragmentShader |
        S::eEarlyFragmentTests | S::eLateFragmentTests | S::eColorAttachmentOutput |
        S::ePreRasterizationShaders | S::eTransformFeedbackEXT;
    return bool(stages & graphics);
}

/// bbport BB_BARRIERS_IN_PASS (default on, =0 to turn it off): see Runtime::FlushBarriers.
bool BarriersInPass() {
    static const bool on = [] {
        const char* env = std::getenv("BB_BARRIERS_IN_PASS");
        return !(env && env[0] == '0');
    }();
    return on;
}
} // namespace

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return vk::ImageType::e1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE_MSG("Unexpected image type {}", u32(type));
    }
}

static std::pair<u32, u32> SanitizeCopyLayers(const VideoCore::ImageInfo& src_info,
                                              const VideoCore::ImageInfo& dst_info,
                                              const u32 depth) {
    const auto vk_src_type = ConvertImageType(src_info.type);
    const auto vk_dst_type = ConvertImageType(dst_info.type);

    u32 src_layers = src_info.resources.layers;
    u32 dst_layers = dst_info.resources.layers;

    // 3D images can only use 1 layer.
    if (vk_src_type == vk::ImageType::e3D && src_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D source layers {} to 1.", src_layers);
        src_layers = 1;
    }
    if (vk_dst_type == vk::ImageType::e3D && dst_layers != 1) {
        LOG_WARNING(Render_Vulkan, "Coercing copy 3D destination layers {} to 1.", dst_layers);
        dst_layers = 1;
    }

    // If the image type is equal, layer count must match. Take the minimum of both.
    if (vk_src_type == vk_dst_type) {
        if (src_layers != dst_layers) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy source layers {} and destination layers {} to minimum.",
                        src_layers, dst_layers);
            src_layers = dst_layers = std::min(src_layers, dst_layers);
        }
    } else {
        // For 2D <-> 3D copies, 2D layer count must equal 3D depth.
        if (vk_src_type == vk::ImageType::e2D && vk_dst_type == vk::ImageType::e3D &&
            src_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D source layers {} to 3D destination depth {}", src_layers,
                        depth);
            src_layers = depth;
        }
        if (vk_src_type == vk::ImageType::e3D && vk_dst_type == vk::ImageType::e2D &&
            dst_layers != depth) {
            LOG_WARNING(Render_Vulkan,
                        "Coercing copy 2D destination layers {} to 3D source depth {}", dst_layers,
                        depth);
            dst_layers = depth;
        }
    }

    return std::make_pair(src_layers, dst_layers);
}

static u32 BufferImageCopySize(const vk::BufferImageCopy& copy, const vk::Format pixel_format) {
    const u32 row_length = copy.bufferRowLength ? copy.bufferRowLength : copy.imageExtent.width;
    const u32 height = copy.bufferImageHeight ? copy.bufferImageHeight : copy.imageExtent.height;

    const auto block = vk::blockExtent(pixel_format);
    const u32 block_size = vk::blockSize(pixel_format);
    const u32 row_pitch = (row_length / block[0]) * block_size;
    const u32 slice_pitch = (height / block[1]) * row_pitch;

    const u32 width_in_blocks = (copy.imageExtent.width + block[0] - 1) / block[0];
    const u32 height_in_blocks = (copy.imageExtent.height + block[1] - 1) / block[1];
    const u32 num_slices = copy.imageExtent.depth * copy.imageSubresource.layerCount;

    return (num_slices - 1) * slice_pitch + (height_in_blocks - 1) * row_pitch +
           width_in_blocks * block_size;
}

Runtime::Runtime(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_}, staging_pool{instance_, scheduler_},
      encoder{&scheduler_.Encoder()} {
    blit_helper = std::make_unique<VideoCore::BlitHelper>(instance, scheduler);
    depth_resampler = std::make_unique<DepthResampler>(instance, scheduler);


    memory_barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    memory_barrier.dstAccessMask =
        vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
}

Runtime::~Runtime() = default;

void Runtime::TickFrame() {
    staging_pool.TickFrame();
}

namespace {
template <typename Out, typename In>
boost::container::small_vector<Out, 8> ConvertRegions(std::span<const In> in) {
    boost::container::small_vector<Out, 8> out;
    out.reserve(in.size());
    for (const In& region : in) {
        out.push_back(ToVk(region));
    }
    return out;
}
} // namespace

void Runtime::CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                         std::span<const VideoCore::Backend::BufferCopy> copies) {
    const auto regions = ConvertRegions<vk::BufferCopy>(copies);
    CopyBuffer(src, dst, std::span<const vk::BufferCopy>{regions});
}

// bbport (BB_HOIST_COPIES): inside a render pass whose recorded draws did not touch the ranges
// (no write to the source, no access to the destination since the last barrier, which a pass
// cannot contain), the copies run before the pass began, then a barrier: the pass is not split
// (a split stores and reloads all its attachments: the G-buffer ~24 times a frame).
bool Runtime::CopyBufferBeforePass(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                                   std::span<const VideoCore::Backend::BufferCopy> copies) {
    if (!Scheduler::HoistCopies() || !scheduler.IsRendering() || copies.empty()) {
        return false;
    }
    static std::atomic<u64> hoisted{0}, conflicts{0}, unplaced{0};
    static const bool stats = std::getenv("BB_FRAME_STATS") != nullptr;
    for (const auto& copy : copies) {
        if (IsBufferAccessed(src, copy.src_offset, copy.size) ||
            IsBufferAccessed(dst, copy.dst_offset, copy.size, true)) {
            if (stats) ++conflicts;
            return false;
        }
    }
    // Copies hoisted before the same pass share one command and one barrier (each command of
    // its own was a Metal blit pass: ~120 a frame), unless one touches what an earlier one wrote
    // or reads: then a new batch, after the earlier one's barrier.
    struct Batch {
        struct Copy {
            vk::Buffer src, dst;
            std::vector<vk::BufferCopy> regions;
        };
        std::vector<Copy> copies;
        std::vector<std::pair<u64, u64>> written, read; ///< by buffer handle: [start, end)
    };
    static std::shared_ptr<Batch> batch;
    static u64 batch_generation = ~0ull;
    const auto overlaps = [](const std::vector<std::pair<u64, u64>>& ranges, u64 start, u64 end) {
        for (const auto& [a, b] : ranges) {
            if (start < b && a < end) return true;
        }
        return false;
    };
    const u64 base_src = reinterpret_cast<u64>(static_cast<VkBuffer>(src->Handle())) << 40;
    const u64 base_dst = reinterpret_cast<u64>(static_cast<VkBuffer>(dst->Handle())) << 40;
    bool joins = batch && batch_generation == scheduler.PassAnchorGeneration();
    for (const auto& copy : copies) {
        if (!joins) break;
        const u64 s0 = base_src + copy.src_offset, d0 = base_dst + copy.dst_offset;
        joins = !overlaps(batch->written, s0, s0 + copy.size) &&
                !overlaps(batch->written, d0, d0 + copy.size) &&
                !overlaps(batch->read, d0, d0 + copy.size);
    }
    const auto note = [&](Batch& b) {
        const auto regions = ConvertRegions<vk::BufferCopy>(copies);
        b.copies.push_back({src->Handle(), dst->Handle(), {regions.begin(), regions.end()}});
        for (const auto& copy : copies) {
            b.read.emplace_back(base_src + copy.src_offset, base_src + copy.src_offset + copy.size);
            b.written.emplace_back(base_dst + copy.dst_offset, base_dst + copy.dst_offset + copy.size);
        }
    };
    bool placed = false;
    if (joins) {
        note(*batch);
        placed = true;
    } else {
        auto fresh = std::make_shared<Batch>();
        note(*fresh);
        placed = scheduler.RecordBeforePass([fresh, enc = encoder](vk::CommandBuffer cmdbuf) {
            for (const auto& copy : fresh->copies) {
                enc->CopyBuffer(cmdbuf, copy.src, copy.dst, copy.regions);
            }
            const vk::MemoryBarrier2 barrier = {
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            };
            enc->Barrier(cmdbuf, vk::DependencyInfo{.memoryBarrierCount = 1,
                                                    .pMemoryBarriers = &barrier});
        });
        if (placed) {
            batch = std::move(fresh);
            batch_generation = scheduler.PassAnchorGeneration();
        }
    }
    if (stats) {
        ++(placed ? hoisted : unplaced);
        if ((hoisted + conflicts + unplaced) % 2000 == 0) {
            std::printf("Copy shader in passes: %llu hoisted, %llu touched by the pass's draws, %llu "
                        "not placeable (pass chunk handed over or no room)\n",
                        (unsigned long long)hoisted.load(), (unsigned long long)conflicts.load(),
                        (unsigned long long)unplaced.load());
        }
    }
    return placed;
}

void Runtime::CopyFromGuestChunk(vk::Buffer src, const VideoCore::Buffer* dst,
                                 std::span<const VideoCore::Backend::BufferCopy> copies) {
    const auto regions = ConvertRegions<vk::BufferCopy>(copies);
    CopyFromGuestChunk(src, dst, std::span<const vk::BufferCopy>{regions});
}

void Runtime::UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                          std::span<const VideoCore::Backend::BufferImageCopy> upload_copies) {
    const auto regions = ConvertRegions<vk::BufferImageCopy>(upload_copies);
    UploadImage(dst, src, std::span<const vk::BufferImageCopy>{regions});
}

void Runtime::DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                            std::span<const VideoCore::Backend::BufferImageCopy> download_copies) {
    const auto regions = ConvertRegions<vk::BufferImageCopy>(download_copies);
    DownloadImage(src, dst, std::span<const vk::BufferImageCopy>{regions});
}

void Runtime::CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                         std::span<const vk::BufferCopy> copies) {
    scheduler.EndRendering();

    // bbport: many regions (HLE copy shaders) are tracked as one bounding range per buffer:
    // conservative for barriers, and two tree lookups instead of two per region.
    const bool bounded = copies.size() > 4;
    u64 src_min = ~0ULL, src_max = 0, dst_min = ~0ULL, dst_max = 0;
    if (bounded) {
        for (const auto& copy : copies) {
            src_min = std::min<u64>(src_min, copy.srcOffset);
            src_max = std::max<u64>(src_max, copy.srcOffset + copy.size);
            dst_min = std::min<u64>(dst_min, copy.dstOffset);
            dst_max = std::max<u64>(dst_max, copy.dstOffset + copy.size);
        }
    }

    bool needs_flush{};
    if (bounded) {
        needs_flush = IsBufferAccessed(src, src_min, src_max - src_min) ||
                      IsBufferAccessed(dst, dst_min, dst_max - dst_min, true);
    } else {
        for (const auto& copy : copies) {
            needs_flush |= IsBufferAccessed(src, copy.srcOffset, copy.size);
            needs_flush |= IsBufferAccessed(dst, copy.dstOffset, copy.size, true);
        }
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([enc = encoder, src_handle = src->Handle(), dst_handle = dst->Handle(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        enc->CopyBuffer(cmdbuf, src_handle, dst_handle, regions);
    });

    if (bounded) {
        AccessBuffer(src, src_min, src_max - src_min, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
        AccessBuffer(dst, dst_min, dst_max - dst_min, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
        return;
    }
    for (const auto& copy : copies) {
        AccessBuffer(src, copy.srcOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
        AccessBuffer(dst, copy.dstOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, size, true)) {
        FlushBarriers();
    }

    scheduler.Record([enc = encoder, handle = dst->Handle(), offset, size,
                      value](vk::CommandBuffer cmdbuf) {
        enc->FillBuffer(cmdbuf, handle, offset, size, value);
    });

    AccessBuffer(dst, offset, size, vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
}

void Runtime::CopyFromGuestChunk(vk::Buffer src, const VideoCore::Buffer* dst,
                                 std::span<const vk::BufferCopy> copies) {
    if (copies.empty()) {
        return;
    }
    scheduler.EndRendering();
    bool needs_flush = false;
    for (const auto& copy : copies) {
        needs_flush |= IsBufferAccessed(dst, copy.dstOffset, copy.size, true);
    }
    if (needs_flush) {
        FlushBarriers();
    }
    scheduler.Record([enc = encoder, src, dst_handle = dst->Handle(),
                      regions = scheduler.RecordData(copies)](vk::CommandBuffer cmdbuf) {
        enc->CopyBuffer(cmdbuf, src, dst_handle, regions);
    });
    for (const auto& copy : copies) {
        AccessBuffer(dst, copy.dstOffset, copy.size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::UpdateBuffer(const VideoCore::Buffer* dst, u64 offset, std::span<const u8> data) {
    if (data.empty()) {
        return;
    }
    scheduler.EndRendering();
    if (IsBufferAccessed(dst, offset, data.size(), true)) {
        FlushBarriers();
    }
    constexpr std::size_t Piece = 32 * 1024; // vkCmdUpdateBuffer takes 64 KiB at most
    for (std::size_t at = 0; at < data.size(); at += Piece) {
        const auto piece = data.subspan(at, std::min(Piece, data.size() - at));
        scheduler.Record([enc = encoder, handle = dst->Handle(), to = offset + at,
                          bytes = scheduler.RecordData(piece)](vk::CommandBuffer cmdbuf) {
            enc->UpdateBuffer(cmdbuf, handle, to, bytes);
        });
    }
    AccessBuffer(dst, offset, data.size(), vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
}

void Runtime::InlineData(VideoCore::Buffer* dst, u64 offset, u32 value) {
    scheduler.EndRendering();

    if (IsBufferAccessed(dst, offset, sizeof(value), true)) {
        FlushBarriers();
    }

    scheduler.Record([enc = encoder, handle = dst->Handle(), offset, value](vk::CommandBuffer cmdbuf) {
        enc->UpdateBuffer(cmdbuf, handle, offset,
                          {reinterpret_cast<const u8*>(&value), sizeof(value)});
    });

    AccessBuffer(dst, offset, sizeof(value), vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite);
}

void Runtime::ImageBarriers(VideoCore::Image& image, vk::ImageLayout dst_layout,
                            vk::AccessFlags2 dst_mask, vk::PipelineStageFlags2 dst_stage,
                            std::optional<VideoCore::SubresourceRange> subres_range) {
    using VideoCore::SubresourceBase;
    auto& barriers = image_barriers;
    const auto& info = image.info;
    const auto aspect_mask = image.aspect_mask;
    ImageState& image_state = ImageStateOf(*image.backing);
    auto& last_state = image_state.state;
    auto& subresource_states = image_state.subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const auto mips =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.level,
                                           subres_range->base.level + subres_range->extent.levels)
                : std::views::iota(0u, info.resources.levels);
        const auto layers =
            needs_partial_transition
                ? std::ranges::views::iota(subres_range->base.layer,
                                           subres_range->base.layer + subres_range->extent.layers)
                : std::views::iota(0u, info.resources.layers);

        for (u32 mip : mips) {
            for (u32 layer : layers) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image.GetImage(),
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
        }
    } else { // Full resource transition
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return;
        }

        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.GetImage(),
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;
}

bool Runtime::Transit(VideoCore::Image* image, VideoCore::Backend::ImageAccess access,
                      std::optional<VideoCore::SubresourceRange> subres_range) {
    const auto state =
        ImageAccessState(access, image->info.props.is_depth, image->info.props.has_stencil,
                         instance.IsAttachmentFeedbackLoopLayoutSupported());
    return Transit(image, state.layout, state.pl_stage, state.access_mask, subres_range);
}

static std::pair<vk::PipelineStageFlags2, vk::AccessFlags2> BufferAccessMasks(
    VideoCore::Backend::BufferAccess access) {
    using VideoCore::Backend::BufferAccess;
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    switch (access) {
    case BufferAccess::GraphicsRead:
        return {Stage::eAllGraphics, Access::eShaderRead};
    case BufferAccess::GraphicsReadWrite:
        return {Stage::eAllGraphics, Access::eShaderRead | Access::eShaderWrite};
    case BufferAccess::ComputeRead:
        return {Stage::eComputeShader, Access::eShaderRead};
    case BufferAccess::ComputeReadWrite:
        return {Stage::eComputeShader, Access::eShaderRead | Access::eShaderWrite};
    case BufferAccess::TransferWrite:
        return {Stage::eTransfer, Access::eTransferWrite};
    }
    UNREACHABLE();
}

void Runtime::AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                           VideoCore::Backend::BufferAccess access) {
    const auto [stage, mask] = BufferAccessMasks(access);
    AccessBuffer(handle, offset, size, stage, mask);
}

void Runtime::AccessGlobal(VideoCore::Backend::BufferAccess access) {
    const auto [stage, mask] = BufferAccessMasks(access);
    AccessGlobal(stage, mask);
}

bool Runtime::Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                      vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                      std::optional<VideoCore::SubresourceRange> subres_range) {
    BeforeImageAccess();
    if (image->pending_clear) [[unlikely]] {
        const auto reason = (dst_access & (vk::AccessFlagBits2::eTransferRead |
                                           vk::AccessFlagBits2::eTransferWrite))
                                ? ClearResolveReason::Copy
            : (dst_access & (vk::AccessFlagBits2::eShaderRead |
                                             vk::AccessFlagBits2::eShaderSampledRead |
                                             vk::AccessFlagBits2::eShaderStorageRead))
                                ? ClearResolveReason::Sample
                                : ClearResolveReason::OtherAccess;
        ResolvePendingClear(image, reason);
    }
    if (native_access_hook) {
        constexpr auto writes =
            vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
            vk::AccessFlagBits2::eColorAttachmentWrite |
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eMemoryWrite;
        native_access_hook(native_access_context, *image, bool(dst_access & writes));
    }
    const size_t prev_num_barriers = static_cast<size_t>(image_barriers.size());
    ImageBarriers(*image, dst_layout, dst_access, dst_stage, subres_range);
    return image_barriers.size() != prev_num_barriers;
}

Runtime::TransferMark::TransferMark(Runtime& runtime, const char* what,
                                    const VideoCore::Image& image) {
    profiler = GpuProfiler::Get();
    if (!profiler || !profiler->Records(&runtime.scheduler)) {
        profiler = nullptr;
        return;
    }
    resume = profiler->Current();
    const auto& info = image.info;
    const u64 key = (u64(reinterpret_cast<uintptr_t>(what)) << 20) ^
                    (u64(info.pixel_format) << 40) ^ (u64(info.size.width) << 16) ^
                    info.size.height ^ 0x7A5Full;
    profiler->Mark(key, [&] {
        return fmt::format("{} {} {}x{}", what, Backend::FormatName(info.pixel_format),
                           info.size.width, info.size.height);
    });
}

Runtime::TransferMark::~TransferMark() {
    if (profiler && resume) {
        profiler->Resume(resume);
    }
}

u64 Runtime::BeginTransferMark(const char* what, const VideoCore::Image& image) {
    auto* profiler = GpuProfiler::Get();
    if (!profiler || !profiler->Records(&scheduler)) {
        return 0;
    }
    const u64 resume = profiler->Current();
    const auto& info = image.info;
    const u64 key = (u64(reinterpret_cast<uintptr_t>(what)) << 20) ^
                    (u64(info.pixel_format) << 40) ^ (u64(info.size.width) << 16) ^
                    info.size.height ^ 0x7A5Full;
    profiler->Mark(key, [&] {
        return fmt::format("{} {} {}x{}", what, Backend::FormatName(info.pixel_format),
                           info.size.width, info.size.height);
    });
    return resume;
}

void Runtime::EndTransferMark(u64 mark) {
    if (auto* profiler = GpuProfiler::Get(); profiler && mark) {
        profiler->Resume(mark);
    }
}

void Runtime::ResampleDepth(u64 src, u32 src_state, u64 dst, u32 dst_state,
                            VideoCore::Backend::Format format, bool stencil, u32 width,
                            u32 height) {
    depth_resampler->Resample(NativeImage(src), vk::ImageLayout(src_state), NativeImage(dst),
                              vk::ImageLayout(dst_state), ToVk(format), stencil,
                              vk::Extent2D{width, height});
}

bool Runtime::CanResampleDepth(VideoCore::Backend::Format format, bool stencil) {
    return depth_resampler->Supported(ToVk(format), stencil);
}

void Runtime::UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                          std::span<const vk::BufferImageCopy> upload_copies) {
    SetBackingSamples(dst, dst->info.num_samples, false);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image upload", *dst};

    bool needs_flush =
        Transit(dst, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferWrite);
    for (const auto& copy : upload_copies) {
        const auto copy_size = BufferImageCopySize(copy, ToVk(dst->info.pixel_format));
        needs_flush |= IsBufferAccessed(src, copy.bufferOffset, copy_size);
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([enc = encoder, src_handle = src->Handle(), image = dst->GetImage(),
                      regions = scheduler.RecordData(upload_copies)](vk::CommandBuffer cmdbuf) {
        enc->CopyBufferToImage(cmdbuf, src_handle, image, regions);
    });

    for (const auto& copy : upload_copies) {
        const auto copy_size = BufferImageCopySize(copy, ToVk(dst->info.pixel_format));
        AccessBuffer(src, copy.bufferOffset, copy_size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferRead);
    }

    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                            std::span<const vk::BufferImageCopy> download_copies) {
    SetBackingSamples(src, src->info.num_samples);
    scheduler.EndRendering();
    const TransferMark mark{*this, "image download", *src};

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    for (const auto& copy : download_copies) {
        const auto copy_size = BufferImageCopySize(copy, ToVk(src->info.pixel_format));
        needs_flush |= IsBufferAccessed(dst, copy.bufferOffset, copy_size, true);
    }
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([enc = encoder, image = src->GetImage(), buffer = dst->Handle(),
                      regions = scheduler.RecordData(download_copies)](vk::CommandBuffer cmdbuf) {
        enc->CopyImageToBuffer(cmdbuf, image, buffer, regions,
                               vk::ImageLayout::eTransferSrcOptimal);
    });

    for (const auto& copy : download_copies) {
        const auto copy_size = BufferImageCopySize(copy, ToVk(src->info.pixel_format));
        AccessBuffer(dst, copy.bufferOffset, copy_size, vk::PipelineStageFlagBits2::eCopy,
                     vk::AccessFlagBits2::eTransferWrite);
    }
}

void Runtime::CopyImage(VideoCore::Image* src, VideoCore::Image* dst) {
    const u32 num_mips = std::min(src->info.resources.levels, dst->info.resources.levels);

    // Format mismatch warning (safe but useful)
    if (src->info.pixel_format != dst->info.pixel_format) {
        LOG_DEBUG(Render_Vulkan,
                  "Copy between different formats: src={}, dst={}. "
                  "Result may be undefined.",
                  Backend::FormatName(src->info.pixel_format), Backend::FormatName(dst->info.pixel_format));
    }

    const u32 base_width = src->info.size.width;
    const u32 base_height = src->info.size.height;
    const u32 base_depth =
        dst->info.type == AmdGpu::ImageType::Color3D ? dst->info.size.depth : src->info.size.depth;

    // Match sample count before copying
    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    boost::container::small_vector<vk::ImageCopy, 8> regions;

    const vk::ImageAspectFlags src_aspect = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    const vk::ImageAspectFlags dst_aspect = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;

    const bool src_is_2d = ConvertImageType(src->info.type) == vk::ImageType::e2D;
    const bool src_is_3d = ConvertImageType(src->info.type) == vk::ImageType::e3D;

    const bool dst_is_2d = ConvertImageType(dst->info.type) == vk::ImageType::e2D;
    const bool dst_is_3d = ConvertImageType(dst->info.type) == vk::ImageType::e3D;

    const bool is_2d_to_3d = src_is_2d && dst_is_3d;
    const bool is_3d_to_2d = src_is_3d && dst_is_2d;
    const bool is_same_type = !is_2d_to_3d && !is_3d_to_2d;

    for (u32 mip = 0; mip < num_mips; ++mip) {
        const u32 mip_w = std::max(base_width >> mip, 1u);
        const u32 mip_h = std::max(base_height >> mip, 1u);
        const u32 mip_d = std::max(base_depth >> mip, 1u);

        const auto [src_layers, dst_layers] = SanitizeCopyLayers(src->info, dst->info, mip_d);

        vk::ImageCopy region{};
        region.srcSubresource.aspectMask = src_aspect;
        region.srcSubresource.mipLevel = mip;
        region.srcSubresource.baseArrayLayer = 0;
        region.dstSubresource.aspectMask = dst_aspect;
        region.dstSubresource.mipLevel = mip;
        region.dstSubresource.baseArrayLayer = 0;

        if (is_same_type) {
            // 2D->2D OR 3D->3D
            if (src_is_3d) {
                // 3D images must use layerCount=1
                region.srcSubresource.layerCount = 1;
                region.dstSubresource.layerCount = 1;
                region.extent = vk::Extent3D(mip_w, mip_h, mip_d);
            } else {
                // Array images
                const u32 copy_layers = std::min(src_layers, dst_layers);
                region.srcSubresource.layerCount = copy_layers;
                region.dstSubresource.layerCount = copy_layers;
                region.extent = vk::Extent3D(mip_w, mip_h, 1);
            }
        } else if (is_2d_to_3d) {
            // 2D array -> 3D volume
            region.srcSubresource.layerCount = src_layers;
            region.dstSubresource.layerCount = 1;
            region.extent = vk::Extent3D(mip_w, mip_h, src_layers);
        } else if (is_3d_to_2d) {
            // 3D volume -> 2D array
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.layerCount = dst_layers;
            region.extent = vk::Extent3D(mip_w, mip_h, dst_layers);
        }

        regions.push_back(region);
    }

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([enc = encoder, src_image = src->GetImage(), dst_image = dst->GetImage(),
                      regions](vk::CommandBuffer cmdbuf) {
        enc->CopyImage(cmdbuf, src_image, dst_image, regions,
                       vk::ImageLayout::eTransferSrcOptimal,
                       vk::ImageLayout::eTransferDstOptimal);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                                  const VideoCore::Buffer* buffer, u64 offset) {
    const u32 num_mips = std::min(src->info.resources.levels, dst->info.resources.levels);
    const u32 num_layers = std::min(src->info.resources.layers, dst->info.resources.layers);
    ASSERT(src->info.resources.layers == dst->info.resources.layers && num_mips == 1);

    SetBackingSamples(dst, dst->info.num_samples, false);
    SetBackingSamples(src, src->info.num_samples);

    vk::BufferImageCopy buffer_copy = {
        .bufferOffset = offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource{
            .aspectMask = src->aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
            .mipLevel = 0u,
            .baseArrayLayer = 0,
            .layerCount = num_layers,
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = {src->info.size.width, src->info.size.height, src->info.size.depth},
    };
    const auto copy_size = BufferImageCopySize(buffer_copy, ToVk(src->info.pixel_format));

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    needs_flush |= IsBufferAccessed(buffer, offset, copy_size);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto download_copy = buffer_copy;
    buffer_copy.imageSubresource.aspectMask = dst->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    scheduler.Record([enc = encoder, src_image = src->GetImage(), dst_image = dst->GetImage(),
                      handle = buffer->Handle(), download_copy,
                      upload_copy = buffer_copy](vk::CommandBuffer cmdbuf) {
        enc->CopyImageToBuffer(cmdbuf, src_image, handle, {&download_copy, 1},
                               vk::ImageLayout::eTransferSrcOptimal);
        const vk::MemoryBarrier2 post_copy_barrier = {
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        };
        enc->Barrier(cmdbuf, vk::DependencyInfo{
                                 .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                                 .memoryBarrierCount = 1u,
                                 .pMemoryBarriers = &post_copy_barrier,
                             });
        enc->CopyBufferToImage(cmdbuf, handle, dst_image, {&upload_copy, 1});
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice) {
    const auto dst_dim = dst->info.props.is_block ? 2 : 0;
    const auto mip_block_w = std::max(dst->info.size.width >> (mip + dst_dim), 1u);
    const auto mip_block_h = std::max(dst->info.size.height >> (mip + dst_dim), 1u);
    const auto mip_block_p = std::max(dst->info.mips_layout[mip].pitch >> dst_dim, 1u);

    const auto src_dim = src->info.props.is_block ? 2 : 0;
    ASSERT(mip_block_w == (src->info.size.width >> src_dim));
    ASSERT(mip_block_h == (src->info.size.height >> src_dim));
    ASSERT(mip_block_p == (src->info.pitch >> src_dim));

    const auto [src_layers, dst_layers] =
        SanitizeCopyLayers(src->info, dst->info, src->info.size.depth);

    const vk::ImageCopy image_copy{
        .srcSubresource{
            .aspectMask = src->aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = src_layers,
        },
        .dstSubresource{
            .aspectMask = src->aspect_mask,
            .mipLevel = mip,
            .baseArrayLayer = slice,
            .layerCount = dst_layers,
        },
        .extent = {src->info.size.width, src->info.size.height, src->info.size.depth},
    };

    SetBackingSamples(dst, dst->info.num_samples);
    SetBackingSamples(src, src->info.num_samples);

    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    scheduler.Record([enc = encoder, src_image = src->GetImage(), dst_image = dst->GetImage(),
                      image_copy](vk::CommandBuffer cmdbuf) {
        enc->CopyImage(cmdbuf, src_image, dst_image, {&image_copy, 1},
                       vk::ImageLayout::eTransferSrcOptimal,
                       vk::ImageLayout::eTransferDstOptimal);
    });

    dst->flags |= (src->flags & VideoCore::ImageFlagBits::GpuModified);
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst) {
    if (src->info.num_samples == 1 && dst->info.num_samples == 1) {
        if (instance.IsMaintenance8Supported() ||
            src->info.props.is_depth == dst->info.props.is_depth) {
            CopyImage(src, dst);
        } else {
            // Perform depth from/to color copy using the intermediate copy buffer.
            static constexpr size_t COPY_BUFFER_SIZE = 128_MB;
            const auto copy_ref =
                staging_pool.Request(COPY_BUFFER_SIZE, VideoCore::MemoryType::DeviceLocal);
            CopyImageWithBuffer(src, dst, copy_ref.buffer, copy_ref.offset);
        }
    } else if (src->info.num_samples == 1 && dst->info.num_samples > 1 &&
               dst->info.props.is_depth) {
        // Perform a rendering pass to transfer the channels of source as samples in dest.
        bool needs_flush =
            Transit(src, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
        needs_flush |= Transit(dst, vk::ImageLayout::eDepthStencilAttachmentOptimal,
                               vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                   vk::PipelineStageFlagBits2::eLateFragmentTests,
                               vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
        if (needs_flush) {
            FlushBarriers();
        }

        blit_helper->ReinterpretColorAsMsDepth(
            dst->info.size.width, dst->info.size.height, dst->info.num_samples,
            src->info.pixel_format, dst->info.pixel_format, src->GetImage(), dst->GetImage());
    } else {
        LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
    }
}

void Runtime::CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                               const VideoCore::SubresourceRange& sub_range) {
    scheduler.EndRendering();

    bool needs_flush =
        Transit(src, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal,
                           vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto aspect_mask = src->aspect_mask & dst->aspect_mask;

    const vk::ImageCopy region = {
        .srcSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = sub_range.base.layer,
            .layerCount = sub_range.extent.layers,
        },
        .srcOffset = {0, 0, 0},
        .dstSubresource{
            .aspectMask = aspect_mask,
            .mipLevel = 0,
            .baseArrayLayer = sub_range.base.layer,
            .layerCount = sub_range.extent.layers,
        },
        .dstOffset = {0, 0, 0},
        .extent = {dst->info.size.width, dst->info.size.height, 1},
    };

    scheduler.Record([enc = encoder, src_image = src->GetImage(), dst_image = dst->GetImage(),
                      region](vk::CommandBuffer cmdbuf) {
        enc->CopyImage(cmdbuf, src_image, dst_image, {&region, 1},
                       vk::ImageLayout::eTransferSrcOptimal,
                       vk::ImageLayout::eTransferDstOptimal);
    });

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                           const VideoCore::SubresourceRange& src_range,
                           const VideoCore::SubresourceRange& dst_range) {
    SetBackingSamples(dst, 1, false);
    scheduler.EndRendering();

    const bool needs_resolve = src->backing->num_samples != 1;
    const auto dst_stage =
        needs_resolve ? vk::PipelineStageFlagBits2::eResolve : vk::PipelineStageFlagBits2::eCopy;

    bool needs_flush = Transit(src, vk::ImageLayout::eTransferSrcOptimal, dst_stage,
                               vk::AccessFlagBits2::eTransferRead);
    needs_flush |= Transit(dst, vk::ImageLayout::eTransferDstOptimal, dst_stage,
                           vk::AccessFlagBits2::eTransferWrite);
    if (needs_flush) {
        FlushBarriers();
    }

    const auto [src_layers, dst_layers] = SanitizeCopyLayers(src->info, dst->info, 1);
    if (!needs_resolve) {
        const vk::ImageCopy region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = src_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = dst_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {src->info.size.width, src->info.size.height, 1},
        };
        scheduler.Record([enc = encoder, src_image = src->GetImage(), dst_image = dst->GetImage(),
                          region](vk::CommandBuffer cmdbuf) {
            enc->CopyImage(cmdbuf, src_image, dst_image, {&region, 1},
                       vk::ImageLayout::eTransferSrcOptimal,
                       vk::ImageLayout::eTransferDstOptimal);
        });
    } else {
        const vk::ImageResolve region = {
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = src_range.base.layer,
                .layerCount = src_layers,
            },
            .srcOffset = {0, 0, 0},
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = dst_range.base.layer,
                .layerCount = dst_layers,
            },
            .dstOffset = {0, 0, 0},
            .extent = {src->info.size.width, src->info.size.height, 1},
        };
        scheduler.Record([enc = encoder, src_image = src->GetImage(), dst_image = dst->GetImage(),
                          region](vk::CommandBuffer cmdbuf) {
            enc->ResolveImage(cmdbuf, src_image, dst_image, region);
        });
    }

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

namespace {
// bbport BB_DEFER_CLEARS: clears noted, taken as load actions, recorded on their own (diagnostics).
std::atomic<u64> clears_deferred{0}, clears_taken{0}, clears_recorded{0};
std::array<std::atomic<u64>, static_cast<size_t>(Runtime::ClearResolveReason::Count)> clear_reasons{};
constexpr std::array<const char*, static_cast<size_t>(Runtime::ClearResolveReason::Count)>
    clear_reason_names{"copy", "sample", "other access", "reduced pass", "upscaler redirect",
                       "view range", "small render area", "multi-level/layer", "backing swap",
                       "superseded", "other"};
bool ClearReasonsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_DEFER_CLEAR_REASONS");
        return env && env[0] == '1';
    }();
    return enabled;
}
} // namespace

bool Runtime::DeferClears() {
    static const bool on = [] {
        const char* env = std::getenv("BB_DEFER_CLEARS");
        return !env || env[0] != '0';
    }();
    return on;
}

void Runtime::ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                         const VideoCore::Backend::ClearColor& clear_value) {
    if (!DeferClears()) {
        RecordClear(dst, range, clear_value);
        return;
    }
    // A later clear of the same range replaces a pending one: nothing used the image between.
    if (dst->pending_clear && dst->pending_clear_range != range) {
        ResolvePendingClear(dst, ClearResolveReason::Superseded);
    }
    dst->pending_clear = true;
    dst->pending_clear_range = range;
    dst->pending_clear_value = clear_value;
    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
    const u64 n = clears_deferred.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & 0x3fff) == 0) {
        std::printf("Deferred clears: %llu, %llu taken as load actions, %llu recorded on their own\n",
                    (unsigned long long)n, (unsigned long long)clears_taken.load(),
                    (unsigned long long)clears_recorded.load());
        if (ClearReasonsEnabled()) {
            std::printf("Deferred clear resolve reasons:");
            for (size_t i = 0; i < clear_reasons.size(); ++i) {
                std::printf(" %s=%llu", clear_reason_names[i],
                            (unsigned long long)clear_reasons[i].load(std::memory_order_relaxed));
            }
            std::printf("\n");
        }
    }
}

void Runtime::ResolvePendingClear(VideoCore::Image* image, ClearResolveReason reason) {
    if (!image->pending_clear) {
        return;
    }
    image->pending_clear = false;
    clears_recorded.fetch_add(1, std::memory_order_relaxed);
    if (ClearReasonsEnabled()) {
        clear_reasons[static_cast<size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    }
    RecordClear(image, image->pending_clear_range, image->pending_clear_value);
}

bool Runtime::TakePendingClear(VideoCore::Image* image, const VideoCore::SubresourceRange& range,
                               VideoCore::Backend::ClearColor& value) {
    if (!image->pending_clear || image->pending_clear_range != range) {
        return false;
    }
    image->pending_clear = false;
    value = image->pending_clear_value;
    clears_taken.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Runtime::RecordClear(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                          const VideoCore::Backend::ClearColor& value) {
    vk::ClearColorValue color{};
    color.uint32 = value;
    scheduler.EndRendering();

    const bool needs_flush =
        Transit(dst, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eClear,
                vk::AccessFlagBits2::eTransferWrite, range);
    if (needs_flush) {
        FlushBarriers();
    }

    const vk::ImageSubresourceRange vk_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = range.base.level,
        .levelCount = range.extent.levels,
        .baseArrayLayer = range.base.layer,
        .layerCount = range.extent.layers,
    };
    scheduler.Record([enc = encoder, image = dst->GetImage(), color,
                      vk_range](vk::CommandBuffer cmdbuf) {
        enc->ClearColorImage(cmdbuf, image, color, vk_range);
    });

    dst->flags |= VideoCore::ImageFlagBits::GpuModified;
    dst->flags &= ~VideoCore::ImageFlagBits::Dirty;
}

void Runtime::SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing) {
    BeforeImageAccess();
    auto& backing = image->backing;
    auto& backing_images = image->backing_images;
    const auto& info = image->info;
    if (!backing || backing->num_samples == num_samples) {
        return;
    }
    ResolvePendingClear(image, ClearResolveReason::BackingSwap); // into the old backing
    ASSERT_MSG(!image->info.props.is_depth, "Swapping samples is only valid for color images");
    VideoCore::Image::BackingImage* new_backing;
    auto it = std::ranges::find(backing_images, num_samples,
                                &VideoCore::Image::BackingImage::num_samples);
    if (it == backing_images.end()) {
        auto new_desc = backing->desc;
        new_desc.samples =
            static_cast<u32>(LiverpoolToVK::NumSamples(num_samples, image->supported_samples));
        if (new_desc.samples > 1) {
            image->AllowedUsage(new_desc); // bbport: multisample backings are never upgraded
        }

        new_backing = &backing_images.emplace_back();
        new_backing->num_samples = num_samples;
        new_backing->desc = new_desc;
        const auto allocation = image->device->CreateImage(
            new_desc, fmt::format("Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{} (backing)",
                                  info.size.width, info.size.height, info.size.depth,
                                  AmdGpu::NameOf(info.tile_mode),
                                  Backend::FormatName(info.pixel_format), info.guest_address,
                                  info.guest_size, info.resources.layers, info.resources.levels,
                                  num_samples));
        new_backing->handle = allocation.handle;
        new_backing->native = allocation.native;
        new_backing->size_bytes = allocation.size_bytes;
        new_backing->backend_state = allocation.backend_state;
    } else {
        new_backing = std::addressof(*it);
    }

    if (copy_backing) {
        scheduler.EndRendering();
        ASSERT(image->info.resources.levels == 1 && image->info.resources.layers == 1);

        // Transition current backing to shader read layout
        Transit(image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);

        // Transition dest backing to color attachment layout, not caring of previous contents
        constexpr auto dst_stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        constexpr auto dst_access =
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
        constexpr auto dst_layout = vk::ImageLayout::eColorAttachmentOptimal;
        image_barriers.push_back(vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = NativeImage(new_backing->native),
            .subresourceRange{
                .aspectMask = image->aspect_mask,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = image->info.resources.layers,
            },
        });
        FlushBarriers();

        // Copy between ms and non ms backing images
        blit_helper->CopyBetweenMsImages(
            info.size.width, info.size.height, new_backing->num_samples, info.pixel_format,
            backing->num_samples > 1, NativeImage(backing->native), NativeImage(new_backing->native));

        // Update current layout in tracker to new backings layout
        ImageState& new_state = ImageStateOf(*new_backing);
        new_state.state.layout = dst_layout;
        new_state.state.access_mask = dst_access;
        new_state.state.pl_stage = dst_stage;
    }

    backing = new_backing;
}

// bbport: lazy texture usages, after Mojahed Abushawish's for bloodborne_mac_port (#33): Apple
// GPUs compress a texture losslessly only without the ShaderWrite and PixelFormatView usages.
// The old backing stays (unused) until the image goes: draws already handed to the recording
// thread may still name it.
void Runtime::UpgradeUsage(VideoCore::Image* image) {
    BeforeImageAccess();
    VideoCore::Image::BackingImage* old_backing = image->backing;
    if (!old_backing || old_backing->num_samples > 1) {
        return;
    }
    ResolvePendingClear(image, ClearResolveReason::BackingSwap); // into the old backing
    const auto& info = image->info;
    auto new_desc = old_backing->desc;
    image->AllowedUsage(new_desc);

    scheduler.EndRendering();
    Transit(image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
            vk::AccessFlagBits2::eTransferRead);
    const auto src_image = image->GetImage();

    auto& new_backing = image->backing_images.emplace_back();
    new_backing.num_samples = old_backing->num_samples;
    new_backing.desc = new_desc;
    const auto allocation = image->device->CreateImage(
        new_desc, fmt::format("Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} (all usages)", info.size.width,
                              info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                              Backend::FormatName(info.pixel_format), info.guest_address,
                              info.guest_size, info.resources.layers, info.resources.levels));
    new_backing.handle = allocation.handle;
    new_backing.native = allocation.native;
    new_backing.size_bytes = allocation.size_bytes;
    new_backing.backend_state = allocation.backend_state;
    old_backing->num_samples = 0; // never chosen again (SetBackingSamples looks for a sample count)
    image->backing = &new_backing;

    Transit(image, vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eCopy,
            vk::AccessFlagBits2::eTransferWrite);
    FlushBarriers();
    boost::container::small_vector<vk::ImageCopy, 16> regions;
    const bool is_3d = new_desc.type == Backend::ImageType::Image3D;
    const vk::ImageAspectFlags aspect = image->aspect_mask & ~vk::ImageAspectFlagBits::eStencil;
    for (u32 level = 0; level < new_desc.levels; ++level) {
        vk::ImageCopy region{};
        region.srcSubresource = {aspect, level, 0, is_3d ? 1u : new_desc.layers};
        region.dstSubresource = region.srcSubresource;
        region.extent = vk::Extent3D(std::max(new_desc.width >> level, 1u), std::max(new_desc.height >> level, 1u),
                                     is_3d ? std::max(new_desc.depth >> level, 1u) : 1u);
        regions.push_back(region);
    }
    scheduler.Record([enc = encoder, src_image, dst_image = image->GetImage(), regions](vk::CommandBuffer cmdbuf) {
        enc->CopyImage(cmdbuf, src_image, dst_image, regions, vk::ImageLayout::eTransferSrcOptimal,
                       vk::ImageLayout::eTransferDstOptimal);
    });
    BbStats::texture_upgrades.fetch_add(1, std::memory_order_relaxed);
    BbStats::texture_upgrade_bytes.fetch_add(new_backing.size_bytes, std::memory_order_relaxed);
}

bool Runtime::IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                               bool check_read_access) {
    const AddressRange range = {
        .resource = reinterpret_cast<u64>(handle),
        .range_start = offset,
        .range_end = offset + size - 1,
    };
    if (global_write) {
        return true; // bbport: a paged write may have touched it (AccessGlobal)
    }
    bool has_access = barrier_tracker.FindRange(range, Access::Write);
    if (check_read_access && !has_access) {
        has_access |= barrier_tracker.FindRange(range, Access::Read);
    }
    return has_access;
}

void Runtime::AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                           vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access) {
    AddressRange range = {
        .resource = reinterpret_cast<u64>(handle),
        .range_start = offset,
        .range_end = offset + size - 1,
    };

    constexpr static vk::AccessFlags2 READ_MASK =
        vk::AccessFlagBits2::eIndexRead | vk::AccessFlagBits2::eVertexAttributeRead |
        vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eShaderRead |
        vk::AccessFlagBits2::eColorAttachmentRead |
        vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eTransferRead |
        vk::AccessFlagBits2::eMemoryRead;

    constexpr static vk::AccessFlags2 WRITE_MASK =
        vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eColorAttachmentWrite |
        vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eTransferWrite |
        vk::AccessFlagBits2::eMemoryWrite | vk::AccessFlagBits2::eTransformFeedbackWriteEXT;

    // bbport: reads are tracked at 4 KiB granularity. Constant data comes from ring allocations
    // at a new offset every draw; rounded, they land in ranges already present and the
    // insert returns early instead of growing the tree until the next barrier flush.
    // Wider read ranges only add barriers, never drop one.
    if (!(src_access & WRITE_MASK) && !BbToggle::Disabled(BbToggle::CoarseReadTracking)) {
        range.range_start &= ~u64{0xFFF};
        range.range_end |= 0xFFF;
    }
    const auto insert = [&](Access access) {
        if (BbToggle::Disabled(BbToggle::AccessMemo)) {
            barrier_tracker.InsertRange(range, access);
            return;
        }
        u64 hash = range.resource ^ range.range_start * 0x9E3779B97F4A7C15ull ^
                   range.range_end << 13 ^ u64(access) << 61;
        hash = (hash ^ hash >> 31) * 0xFF51AFD7ED558CCDull;
        auto& memo = access_memo[(hash ^ hash >> 29) % access_memo.size()];
        if (memo.epoch == access_epoch && memo.resource == range.resource &&
            memo.start == range.range_start && memo.end == range.range_end &&
            memo.access == u32(access)) {
            return;
        }
        barrier_tracker.InsertRange(range, access);
        memo = {range.resource, range.range_start, range.range_end, access_epoch, u32(access)};
    };
    if (src_access & WRITE_MASK) {
        insert(Access::Write);
        graphics_write |= IsGraphicsStage(src_stage);
    }
    if (src_access & READ_MASK) {
        insert(Access::Read);
    }

    memory_barrier.srcStageMask |= src_stage;
    memory_barrier.srcAccessMask |= src_access & WRITE_MASK;
}

void Runtime::AccessGlobal(vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access) {
    memory_barrier.srcStageMask |= src_stage;
    if (src_access & vk::AccessFlagBits2::eShaderWrite) {
        memory_barrier.srcAccessMask |= vk::AccessFlagBits2::eShaderWrite;
        global_write = true;
        graphics_write |= IsGraphicsStage(src_stage);
    }
}

u32 Runtime::BindingLayout(const VideoCore::Image& image) const {
    return static_cast<u32>(ImageStateOf(*image.backing).state.layout);
}

void Runtime::FlushBarriers() {
    BeforeImageAccess();
    vk::DependencyInfo dep_info{};
    // bbport (BB_BARRIERS_IN_PASS): on the native Metal renderer a barrier encodes nothing
    // (MetalFrame::Barrier: Metal orders the encoders), so ending the open render pass for one
    // only stores and reloads its attachments. The pass stays open unless its own draws may have
    // written what is waited for (a graphics-stage write since the last flush) or an image
    // barrier is on one of its attachments (sampling it needs the pass stored first).
    bool keep_pass = false;
#if BB_METAL_BACKEND
    if (BarriersInPass() && !graphics_write && scheduler.IsRendering() &&
        MetalLayer::FrameEnabled() && (memory_barrier.srcStageMask || !image_barriers.empty())) {
        keep_pass = true;
        if (!image_barriers.empty()) {
            const RenderState& state = scheduler.GetRenderState();
            std::array<VkImage, 9> images{};
            u32 n = 0;
            for (u32 i = 0; i < state.num_color_attachments; ++i) {
                images[n++] = VkImage(MetalLayer::ImageOfView(state.color_attachments[i].image_view));
            }
            if (state.depth_stencil_attachment.image_view) {
                images[n++] =
                    VkImage(MetalLayer::ImageOfView(state.depth_stencil_attachment.image_view));
            }
            for (const auto& barrier : image_barriers) {
                const VkImage image = static_cast<VkImage>(barrier.image);
                for (u32 i = 0; i < n; ++i) {
                    // An attachment whose image is not known counts as this image.
                    keep_pass &= images[i] && images[i] != image;
                }
            }
        }
    }
#endif
    graphics_write = false;

    if (memory_barrier.srcStageMask) {
        dep_info.pMemoryBarriers = &memory_barrier;
        dep_info.memoryBarrierCount = 1U;
    }
    if (!image_barriers.empty()) {
        dep_info.pImageMemoryBarriers = image_barriers.data();
        dep_info.imageMemoryBarrierCount = static_cast<u32>(image_barriers.size());
    }

    if (!dep_info.memoryBarrierCount && !dep_info.imageMemoryBarrierCount) {
        return;
    }

    if (!keep_pass) {
        scheduler.EndRendering();
    }
    scheduler.Record([enc = encoder, memory = memory_barrier,
                      has_memory = dep_info.memoryBarrierCount != 0,
                      images = scheduler.RecordData(std::span<const vk::ImageMemoryBarrier2>(
                          image_barriers.data(), image_barriers.size()))](vk::CommandBuffer cmdbuf) {
        const vk::DependencyInfo info = {
            .memoryBarrierCount = has_memory ? 1U : 0U,
            .pMemoryBarriers = has_memory ? &memory : nullptr,
            .imageMemoryBarrierCount = static_cast<u32>(images.size()),
            .pImageMemoryBarriers = images.data(),
        };
        enc->Barrier(cmdbuf, info);
    });

    memory_barrier.srcStageMask = vk::PipelineStageFlagBits2::eNone;
    memory_barrier.srcAccessMask = vk::AccessFlagBits2::eNone;

    image_barriers.clear();
    barrier_tracker.Clear();
    global_write = false;
    ++access_epoch;
}

} // namespace Vulkan
