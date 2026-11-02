// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_backend/transfers.h"
#include "video_core/renderer_vulkan/vk_barrier_tracker.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_stream_encoder.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class BlitHelper;
} // namespace VideoCore

namespace Vulkan {

class DepthResampler;
class GpuProfiler;

class Instance;
class Scheduler;

/// The Vulkan backend's transfers and declared uses (renderer_backend/transfers.h).
class Runtime final : public VideoCore::Backend::Transfers {
public:
    explicit Runtime(const Instance& instance, Scheduler& scheduler);
    ~Runtime();

    Scheduler& GetScheduler() {
        return scheduler;
    }

    const Instance& GetInstance() const {
        return instance;
    }

    StagingBufferPool& GetStagingPool() override {
        return staging_pool;
    }

    void TickFrame() override;

    // The render adapter's regions (docs/RENDER_ADAPTER.md, A3), converted where recorded.
    void CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                    std::span<const VideoCore::Backend::BufferCopy> copies) override;
    [[nodiscard]] bool CopyBufferBeforePass(
        const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
        std::span<const VideoCore::Backend::BufferCopy> copies) override;
    void CopyFromGuestChunk(u64 src, const VideoCore::Buffer* dst,
                            std::span<const VideoCore::Backend::BufferCopy> copies) override {
        CopyFromGuestChunk(vk::Buffer{reinterpret_cast<VkBuffer>(src)}, dst, copies);
    }
    void CopyFromGuestChunk(vk::Buffer src, const VideoCore::Buffer* dst,
                            std::span<const VideoCore::Backend::BufferCopy> copies);
    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const VideoCore::Backend::BufferImageCopy> upload_copies) override;
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const VideoCore::Backend::BufferImageCopy> download_copies) override;

    void CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                    std::span<const vk::BufferCopy> copies);

    void FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value) override;

    void InlineData(VideoCore::Buffer* dst, u64 offset, u32 value);
    /// bbport BB_GUEST_IN_PLACE: copies from the game's memory itself (a guest memory chunk's
    /// buffer, host memory the CPU writes) into `dst`: uploads without a CPU copy.
    void CopyFromGuestChunk(vk::Buffer src, const VideoCore::Buffer* dst,
                            std::span<const vk::BufferCopy> copies);
    /// bbport: `data` written into `dst` by the GPU in stream order (vkCmdUpdateBuffer pieces).
    void UpdateBuffer(const VideoCore::Buffer* dst, u64 offset, std::span<const u8> data) override;

    /// The render adapter's declaration of a use (docs/RENDER_ADAPTER.md): the layout, stages
    /// and accesses follow from it and the image (depth, stencil, feedback-loop support).
    bool Transit(VideoCore::Image* image, VideoCore::Backend::ImageAccess access,
                 std::optional<VideoCore::SubresourceRange> subres_range = {}) override;
    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                      VideoCore::Backend::BufferAccess access) override;
    void AccessGlobal(VideoCore::Backend::BufferAccess access) override;

    bool Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                 std::optional<VideoCore::SubresourceRange> subres_range = {});

    /// BB_GPU_PROFILE: a profiler segment for a transfer of `image` while it lives; the
    /// segment it interrupted continues afterwards.
    class TransferMark {
    public:
        TransferMark(Runtime& runtime, const char* what, const VideoCore::Image& image);
        ~TransferMark();
        TransferMark(const TransferMark&) = delete;
        TransferMark& operator=(const TransferMark&) = delete;

    private:
        GpuProfiler* profiler = nullptr;
        u64 resume = 0;
    };
    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const vk::BufferImageCopy> upload_copies);
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferImageCopy> download_copies);

    void CopyImage(VideoCore::Image* src, VideoCore::Image* dst) override;
    void CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                             const VideoCore::Buffer* buffer, u64 offset);
    void CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice) override;

    void CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst) override;

    void CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                          const VideoCore::SubresourceRange& sub_range) override;

    void ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                      const VideoCore::SubresourceRange& src_range,
                      const VideoCore::SubresourceRange& dst_range) override;
    void ResampleDepth(u64 src, u32 src_state, u64 dst, u32 dst_state,
                       VideoCore::Backend::Format format, bool stencil, u32 width,
                       u32 height) override;
    [[nodiscard]] bool CanResampleDepth(VideoCore::Backend::Format format, bool stencil) override;
    void SetNativeAccessHook(void (*hook)(void*, VideoCore::Image&, bool), void* context) override {
        native_access_hook = hook;
        native_access_context = context;
    }
    [[nodiscard]] u64 BeginTransferMark(const char* what, const VideoCore::Image& image) override;
    void EndTransferMark(u64 mark) override;
    void ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                    const VideoCore::Backend::ClearColor& clear_value) override;
    /// bbport BB_DEFER_CLEARS (default on, =0 to disable): ClearImage notes the clear on the
    /// image; a render pass that draws into the whole cleared range takes it as its load action
    /// (TakePendingClear), and
    /// anything else that uses the image records it first (ResolvePendingClear, from Transit).
    /// A clear between passes is a render pass of its own on tile-based GPUs.
    static bool DeferClears();
    using ClearResolveReason = VideoCore::Backend::Transfers::ClearResolveReason;
    void ResolvePendingClear(VideoCore::Image* image,
                             ClearResolveReason reason = ClearResolveReason::Other) override;
    bool TakePendingClear(VideoCore::Image* image, const VideoCore::SubresourceRange& range,
                          VideoCore::Backend::ClearColor& value) override;

    void SetBackingSamples(VideoCore::Image* image, u32 num_samples,
                           bool copy_backing = true) override;
    void UpgradeUsage(VideoCore::Image* image) override;

    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                      vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access);

    bool IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                          bool check_read_access = false) override;

    /// bbport BB_LAYER_MEMORY: a shader reached memory through the page table (any buffer of the
    /// game's memory). A write there makes every later access wait for a barrier.
    void AccessGlobal(vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access);

    void FlushBarriers() override;
    [[nodiscard]] u32 BindingLayout(const VideoCore::Image& image) const override;

    /// bbport: runs before this thread changes image state (layouts, pending image
    /// barriers): the rasterizer joins its texture binding helper there (BindHelper).
    void SetImageAccessHook(void (*hook)(void*), void* context) override {
        image_access_hook = hook;
        image_access_context = context;
    }
    void BeforeImageAccess() override {
        if (image_access_hook) {
            image_access_hook(image_access_context);
        }
    }

private:
    /// Appends the barriers that bring `image`'s current backing to the given use (its state is
    /// the backend's ImageState).
    void ImageBarriers(VideoCore::Image& image, vk::ImageLayout dst_layout,
                       vk::AccessFlags2 dst_mask, vk::PipelineStageFlags2 dst_stage,
                       std::optional<VideoCore::SubresourceRange> subres_range);
    void RecordClear(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                     const VideoCore::Backend::ClearColor& value);
    void (*image_access_hook)(void*) = nullptr;
    void* image_access_context = nullptr;
    void (*native_access_hook)(void*, VideoCore::Image&, bool) = nullptr;
    void* native_access_context = nullptr;
    std::unique_ptr<DepthResampler> depth_resampler;
    /// How recorded transfers encode: the scheduler's (the Metal frame's own with
    /// BB_METAL_FRAME=1), set before the runtime exists.
    const StreamEncoder* encoder;
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
    BarrierTracker barrier_tracker;
    boost::container::small_vector<vk::ImageMemoryBarrier2, 32> image_barriers;
    vk::MemoryBarrier2 memory_barrier{};
    bool global_write = false; ///< bbport: AccessGlobal wrote since the last FlushBarriers
    /// bbport: a graphics stage wrote since the last FlushBarriers (barriers kept inside passes).
    bool graphics_write = false;
    // bbport: ranges inserted into barrier_tracker since its last Clear(); re-inserting a
    // contained range is a no-op, and draws re-bind the same ranges constantly.
    struct AccessMemo {
        u64 resource;
        u64 start;
        u64 end;
        u32 epoch;
        u32 access;
    };
    std::array<AccessMemo, 512> access_memo{};
    u32 access_epoch = 1;
};

} // namespace Vulkan
