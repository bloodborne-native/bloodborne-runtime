// SPDX-License-Identifier: GPL-2.0-or-later
// From Wooloomooloo2/bloodborne_mac_port (78b59f5, GPL-2.0-or-later), adapted to bloodborne-runtime.
// bbport: the Metal frame encoder (docs/DESIGN_METAL_BACKEND.md, M2 step C). The Vulkan layer
// (mtl_vk_layer.mm) passes every command of a command buffer the Metal path owns to these
// functions, which encode it into one MTLCommandBuffer on MoltenVK's queue. Objective-C++ only.

#pragma once

#ifndef __OBJC__
#error "mtl_frame.h is for Objective-C++ (.mm) files only"
#endif

#import <Metal/Metal.h>

#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
// Metal export types (this directory builds without VK_USE_PLATFORM_METAL_EXT).
#include <vulkan/vulkan_metal.h>
#include "common/types.h"

namespace MetalShadow {
struct GraphicsEntry;
}
namespace Metal {
struct ComputePipeline;
}

namespace MetalFrame {

/// One descriptor of a push descriptor set (by binding, then array element).
struct Bound {
    id<MTLBuffer> buffer;
    u64 offset{};
    u64 size{};
    id<MTLTexture> texture;
    id<MTLSamplerState> sampler;
};

struct Binding {
    std::vector<Bound> elements; ///< array elements
};

struct PendingPatch;

/// What the open render encoder already has (set only on change; reset per encoder).
struct EncoderCache {
    void* pipeline{};
    void* depth_stencil{};
    u32 fill{~0u}, clip{~0u}, cull{~0u}, winding{~0u};
    u64 viewport_hash{}, scissor_hash{};
    u32 ref_front{~0u}, ref_back{~0u};
    float bias[3]{-1.0f, -1.0f, -1.0f};
    float blend[4]{-1.0f, -1.0f, -1.0f, -1.0f};
    struct Slot {
        void* object{};
        u64 offset{};
    };
    std::array<Slot, 31> buffers[2]{};   ///< vertex, fragment
    std::array<void*, 32> textures[2]{};
    std::array<void*, 16> samplers[2]{};
};

/// What the frame encoder keeps for one owned command buffer.
struct Owned {
    id<MTLCommandBuffer> cmd; ///< unretained references: the Vulkan objects keep resources alive
    std::vector<id<MTLBuffer>> keep; ///< our temporary buffers, released when the GPU is done
    u32 chunk_draws{}; ///< draws since the last partial commit (CommitChunk)
    bool last_draw_tess{}; ///< the previous draw of this pass was tessellated (statistics)
    std::vector<PendingPatch> pending; ///< patch draws of the current tessellation run
    id<MTLRenderCommandEncoder> render;
    EncoderCache cache; ///< of `render`
    id<MTLComputeCommandEncoder> compute;
    id<MTLBlitCommandEncoder> blit;
    unsigned commands{};

    // Render pass (vkCmdBeginRendering .. vkCmdEndRendering). `pass` reopens it (loading, not
    // clearing) after work that had to interrupt it (tessellation kernels).
    bool rendering{};
    MTLRenderPassDescriptor* pass;       ///< the pass with Load actions (reopening)
    MTLRenderPassDescriptor* pass_first; ///< as begun (its clears); opened at the first draw
    bool pass_opened{};                  ///< an encoder of this pass was created
    bool pass_clears{};                  ///< pass_first clears something
    // Fenced ordering (BB_METAL_FENCED_ORDERING): render encoders do not declare the guest memory;
    // fences order them against compute and blit encoders instead.
    bool nonrender_seen{};                   ///< a compute or blit encoder was opened here
    bool war_pending{};                      ///< a barrier since the last compute/blit wait
    std::vector<id<MTLFence>> render_fences; ///< updated by render encoders since that wait
    std::vector<id<MTLFence>> fence_pool;
    size_t fence_next{};
    // Scratch memory for tessellation (bump-allocated, kept until the GPU is done).
    id<MTLBuffer> scratch;
    u64 scratch_used{};
    std::vector<id<MTLBuffer>> scratch_used_buffers; ///< back to the pool when the GPU is done
    u32 target_width{}, target_height{};

    // Bound state.
    std::shared_ptr<const MetalShadow::GraphicsEntry> graphics;
    const Metal::ComputePipeline* compute_pipeline{};
    bool graphics_bound{}; ///< a graphics pipeline was bound (even one without a Metal pipeline)
    std::array<u8, 256> push{};
    /// Push descriptors by (set << 16 | binding), per bind point.
    std::unordered_map<u32, Binding> graphics_bindings, compute_bindings;

    struct VertexBuffer {
        id<MTLBuffer> buffer;
        u64 offset{};
        u64 stride{};
    };
    std::array<VertexBuffer, 32> vertex_buffers{};
    id<MTLBuffer> index_buffer;
    u64 index_offset{};
    VkIndexType index_type{VK_INDEX_TYPE_UINT16};

    // Dynamic state (as the Vulkan side set it).
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    VkCullModeFlags cull_mode{};
    VkFrontFace front_face{VK_FRONT_FACE_COUNTER_CLOCKWISE};
    bool depth_test{}, depth_write{};
    VkCompareOp depth_compare{VK_COMPARE_OP_ALWAYS};
    bool stencil_test{};
    struct Face {
        VkStencilOp fail{}, pass{}, depth_fail{};
        VkCompareOp compare{VK_COMPARE_OP_ALWAYS};
        u32 compare_mask{0xff}, write_mask{0xff}, reference{};
    } front, back;
    bool depth_bias_enable{};
    float depth_bias[3]{}; ///< constant, clamp, slope
    float blend_constants[4]{};
    bool rasterizer_discard{};
};

/// The draw state a deferred patch draw needs (tessellation runs, FlushPending).
struct DrawSnap {
    std::shared_ptr<const MetalShadow::GraphicsEntry> graphics;
    std::array<u8, 256> push{};
    std::unordered_map<u32, Binding> bindings;
    std::vector<VkViewport> viewports;
    std::vector<VkRect2D> scissors;
    VkCullModeFlags cull_mode{};
    VkFrontFace front_face{};
    bool depth_test{}, depth_write{}, stencil_test{}, depth_bias_enable{}, rasterizer_discard{};
    VkCompareOp depth_compare{};
    Owned::Face front, back;
    float depth_bias[3]{};
    float blend_constants[4]{};
};

/// A tessellated draw whose kernels ran and whose patch draw waits for the end of the run.
struct PendingPatch {
    DrawSnap snap;
    id<MTLBuffer> scratch;
    u64 control_offset{}, patch_offset{}, factor_offset{};
    u32 patches{}, output_control_points{};
    u32 instance{}; ///< the draw's instance (base instance of the patch draw)
};

/// Encodes the waiting patch draws (in the reopened pass). Every command except state setup and
/// another tessellated draw calls it first.
void FlushPending(Owned& o);

/// The device, queue and export entry point (from the layer's SetDevice).
void Init(VkDevice device, id<MTLCommandQueue> queue, PFN_vkExportMetalObjectsEXT export_objects,
          PFN_vkGetBufferDeviceAddress get_address);

/// Ends whatever encoder is open (before the command buffer is committed, at barriers, ...).
void EndEncoders(Owned& o);

/// Commits what is encoded so far and continues in a new command buffer (same queue, so in
/// order): the GPU starts on the frame while the rest is recorded. Also hands `keep` to the
/// committed one.
void CommitChunk(Owned& o);
/// Fenced ordering: keeps this command buffer's fences until the GPU is done with it.
void RetainFencesUntilDone(Owned& o);

/// Hands the command buffer's scratch buffers back to the pool once the GPU is done with it
/// (before commit).
void ReturnScratchWhenDone(Owned& o);

// Resource bookkeeping from the device functions the layer sees.
void SamplerCreated(VkSampler sampler, const VkSamplerCreateInfo* info);
void SamplerDestroyed(VkSampler sampler);
void ImageViewCreated(VkImageView view, const VkImageViewCreateInfo* info);
void ImageViewDestroyed(VkImageView view);
void DeviceAddressTaken(VkBuffer buffer);
/// The device's buffers (MetalLayer::BufferMade): their MTLBuffer and offset, looked up once.
void BufferMade(VkBuffer buffer);
void BufferGone(VkBuffer buffer);
/// A buffer only Metal has (placeholder `buffer`, `mtl` at offset 0).
void MetalBufferMade(VkBuffer buffer, id<MTLBuffer> mtl);
/// A registered buffer's device address (gpuAddress plus its offset), 0 when unknown.
u64 BufferAddress(VkBuffer buffer);
/// Images' textures (M4): `texture` for an image only Metal has, nil to export a MoltenVK image's
/// once. ImageGone drops the record.
void ImageMade(VkImage image, id<MTLTexture> texture);
void ImageGone(VkImage image);
/// A view of an image only Metal has, made in Metal and recorded under `view` (a placeholder);
/// false when Metal cannot make it.
bool MakeView(VkImageView view, const VkImageViewCreateInfo* info);

// Commands.
void Barrier(Owned& o);
void BeginRendering(Owned& o, const VkRenderingInfo* info);
void EndRendering(Owned& o);
void BindPipeline(Owned& o, VkPipelineBindPoint point, VkPipeline pipeline);
void PushDescriptorSet(Owned& o, VkPipelineBindPoint point, u32 set, u32 count,
                       const VkWriteDescriptorSet* writes);
void PushConstants(Owned& o, u32 offset, u32 size, const void* values);
/// Descriptor sets (the overlay, FSR): vkUpdateDescriptorSets contents, applied at bind.
void DescriptorSetsUpdated(u32 count, const VkWriteDescriptorSet* writes);
void BindDescriptorSets(Owned& o, VkPipelineBindPoint point, u32 first_set, u32 count,
                        const VkDescriptorSet* sets);
void BindVertexBuffers(Owned& o, u32 first, u32 count, const VkBuffer* buffers,
                       const VkDeviceSize* offsets, const VkDeviceSize* strides);
void BindIndexBuffer(Owned& o, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
void SetViewports(Owned& o, u32 count, const VkViewport* viewports);
void SetScissors(Owned& o, u32 count, const VkRect2D* scissors);
void Draw(Owned& o, u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance);
void DrawIndexed(Owned& o, u32 index_count, u32 instance_count, u32 first_index,
                 s32 vertex_offset, u32 first_instance);
void DrawIndirect(Owned& o, VkBuffer buffer, VkDeviceSize offset, u32 count, u32 stride,
                  bool indexed);
void Dispatch(Owned& o, u32 x, u32 y, u32 z);
void DispatchIndirect(Owned& o, VkBuffer buffer, VkDeviceSize offset);
void CopyBuffer(Owned& o, VkBuffer src, VkBuffer dst, u32 count, const VkBufferCopy* regions);
void FillBuffer(Owned& o, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, u32 data);
void UpdateBuffer(Owned& o, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size,
                  const void* data);
void CopyImage(Owned& o, VkImage src, VkImage dst, u32 count, const VkImageCopy* regions);
void CopyBufferToImage(Owned& o, VkBuffer src, VkImage dst, u32 count,
                       const VkBufferImageCopy* regions);
void CopyImageToBuffer(Owned& o, VkImage src, VkBuffer dst, u32 count,
                       const VkBufferImageCopy* regions);
void ClearColorImage(Owned& o, VkImage image, const VkClearColorValue* color, u32 count,
                     const VkImageSubresourceRange* ranges);
void ClearDepthStencilImage(Owned& o, VkImage image, const VkClearDepthStencilValue* value,
                            u32 count, const VkImageSubresourceRange* ranges);
void BlitImage(Owned& o, VkImage src, VkImage dst, u32 count, const VkImageBlit* regions,
               VkFilter filter);

/// The Metal texture of a Vulkan image (exported from MoltenVK, or the one it was made from).
id<MTLTexture> ImageTexture(VkImage image);

/// Buffers declared on every encoder (device-address use), for the reports.
size_t AddressBufferCount();

/// Counters for BB_FRAME_STATS-style reports (skipped work by reason).
struct Stats {
    std::atomic<u64> draws{}, dispatches{}, skipped_no_pipeline{}, skipped_tess{},
        skipped_emulated{}, skipped_discard{}, unsupported{}, tess_draws{}, tess_after_tess{};
};
Stats& GetStats();
/// The image of a view (VK_NULL_HANDLE: not known).
VkImage ImageOfView(VkImageView view);

} // namespace MetalFrame
