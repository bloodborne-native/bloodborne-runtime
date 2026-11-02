// SPDX-License-Identifier: GPL-2.0-or-later
// From Wooloomooloo2/bloodborne_mac_port (78b59f5, GPL-2.0-or-later), adapted to bloodborne-runtime.

#include <dlfcn.h>
#include <xxhash.h>
#include <mach-o/dyld.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <set>
#include "video_core/renderer_metal/mtl_frame.h"
#include <vulkan/vulkan_metal.h>
#include "video_core/renderer_metal/mtl_depth_stencil.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_metal/mtl_raster_util.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_shadow.h"
#include "video_core/renderer_metal/mtl_tessellation.h"
#include "video_core/renderer_metal/mtl_geometry.h"
#include "video_core/renderer_metal/vk_to_mtl.h"

namespace MetalFrame {
namespace {

VkDevice vk_device;
id<MTLCommandQueue> queue;
id<MTLDevice> device;
PFN_vkExportMetalObjectsEXT export_objects;
PFN_vkGetBufferDeviceAddress get_address;
using GetMTLBuffer = void (*)(VkBuffer, id<MTLBuffer>*);
GetMTLBuffer get_mtl_buffer;
Stats stats;

id<MTLBuffer> null_buffer;
id<MTLTexture> null_texture;
id<MTLSamplerState> null_sampler;

std::mutex scratch_pool_mutex;
std::vector<id<MTLBuffer>> scratch_pool; ///< 8 MiB scratch buffers the GPU is done with

std::mutex maps_mutex;
std::unordered_map<VkSampler, id<MTLSamplerState>> samplers;
std::unordered_map<VkImageView, id<MTLTexture>> views;
/// What each image view shows (for attachments: Metal renders into the image's own texture, the
/// exported view texture may lack render-target usage).
struct ViewInfo {
    VkImage image;
    VkFormat format;
    VkImageSubresourceRange range;
};
std::unordered_map<VkImageView, ViewInfo> view_infos;
/// Buffers the shaders reach by device address (the guest arena, the BDA page table, ...): every
/// encoder declares them (useResources), which makes them resident and hazard-tracked, as MoltenVK
/// does for buffers with device addresses.
std::vector<id<MTLBuffer>> address_buffers;
/// The big ones (the guest arena, >= 64 MiB) are what shaders reach through pointers and write:
/// only they are declared on each encoder (hazard tracking); all of them are kept resident by a
/// residency set on the queue, which pointer access needs and which costs nothing per encoder.
std::vector<id<MTLBuffer>> declared_buffers;
id<MTLResidencySet> residency;
std::atomic<u32> address_generation{0};
constexpr NSUInteger kDeclaredMinBytes = 64ull << 20;

void Log(const char* what) {
    static std::mutex mutex;
    static std::vector<std::string> seen;
    std::scoped_lock lock{mutex};
    if (std::find(seen.begin(), seen.end(), what) == seen.end()) {
        seen.emplace_back(what);
        std::printf("Metal frame: %s\n", what);
    }
}

/// MoltenVK's vkGetMTLBufferMVK (its private API, not reachable through the Vulkan loader).
GetMTLBuffer FindGetMTLBuffer() {
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const char* name = _dyld_get_image_name(i);
        if (name && std::strstr(name, "libMoltenVK")) {
            if (void* handle = dlopen(name, RTLD_NOLOAD | RTLD_LAZY)) {
                return reinterpret_cast<GetMTLBuffer>(dlsym(handle, "vkGetMTLBufferMVK"));
            }
        }
    }
    return nullptr;
}

struct BufferRef {
    id<MTLBuffer> buffer;
    u64 offset{};
};

/// The MTLBuffer behind a VkBuffer and where the VkBuffer starts in it (MoltenVK suballocates
/// buffers from device memory; the device address gives the offset), asked of MoltenVK.
BufferRef ResolveFromDriver(VkBuffer buffer) {
    BufferRef ref;
    if (!buffer || !get_mtl_buffer) {
        return ref;
    }
    id<MTLBuffer> mtl = nil;
    get_mtl_buffer(buffer, &mtl);
    if (!mtl) {
        return ref;
    }
    ref.buffer = mtl;
    if (get_address && mtl.gpuAddress) {
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer = buffer;
        const u64 address = get_address(vk_device, &info);
        if (address >= mtl.gpuAddress && address < mtl.gpuAddress + mtl.length) {
            ref.offset = address - mtl.gpuAddress;
        }
    }
    return ref;
}

/// The device's buffers, resolved once when they are made (BufferMade) and dropped right before
/// they are destroyed (BufferGone): VMA destroys buffers without the dispatcher, so only the
/// device can say when an entry is stale. Anything else is asked of MoltenVK on every use.
std::shared_mutex registered_mutex;
std::unordered_map<VkBuffer, BufferRef> registered;
std::atomic<u64> registered_generation{0};

BufferRef ResolveBuffer(VkBuffer buffer) {
    // The same few buffers (the guest pool, stream buffers) are bound over and over.
    thread_local VkBuffer cached_buffer = VK_NULL_HANDLE;
    thread_local __unsafe_unretained id<MTLBuffer> cached_mtl = nil;
    thread_local u64 cached_offset = 0, cached_generation = ~0ull;
    const u64 generation = registered_generation.load(std::memory_order_acquire);
    if (buffer == cached_buffer && generation == cached_generation) {
        return {cached_mtl, cached_offset};
    }
    {
        std::shared_lock lock{registered_mutex};
        if (auto it = registered.find(buffer); it != registered.end()) {
            cached_buffer = buffer;
            cached_mtl = it->second.buffer;
            cached_offset = it->second.offset;
            cached_generation = generation;
            return it->second;
        }
    }
    return ResolveFromDriver(buffer);
}

id<MTLTexture> ExportTexture(VkImage image, VkImageView view) {
    VkExportMetalTextureInfoEXT texture{VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
    texture.image = image;
    texture.imageView = view;
    texture.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &texture};
    export_objects(vk_device, &info);
    return texture.mtlTexture;
}

id<MTLTexture> TextureOfView(VkImageView view) {
    if (!view) {
        return nil;
    }
    {
        std::scoped_lock lock{maps_mutex};
        if (auto it = views.find(view); it != views.end()) {
            return it->second;
        }
    }
    id<MTLTexture> texture = ExportTexture(VK_NULL_HANDLE, view);
    std::scoped_lock lock{maps_mutex};
    views[view] = texture;
    return texture;
}

/// Images' textures (M4): those only Metal has, and the device's MoltenVK images exported once.
/// Anything else is exported on every use.
std::shared_mutex images_mutex;
std::unordered_map<VkImage, id<MTLTexture>> image_textures;
std::atomic<u64> images_generation{0};

id<MTLTexture> TextureOfImage(VkImage image) {
    if (!image) {
        return nil;
    }
    // Copies, clears and blits name the same images over and over.
    thread_local VkImage cached_image = VK_NULL_HANDLE;
    thread_local __unsafe_unretained id<MTLTexture> cached_texture = nil;
    thread_local u64 cached_generation = ~0ull;
    const u64 generation = images_generation.load(std::memory_order_acquire);
    if (image == cached_image && generation == cached_generation) {
        return cached_texture;
    }
    {
        std::shared_lock lock{images_mutex};
        if (auto it = image_textures.find(image); it != image_textures.end()) {
            cached_image = image;
            cached_texture = it->second;
            cached_generation = generation;
            return it->second;
        }
    }
    return ExportTexture(image, VK_NULL_HANDLE);
}

// ---- Encoders ----

void UseAddressBuffers(id<MTLCommandEncoder> encoder, bool render) {
    std::scoped_lock lock{maps_mutex};
    if (declared_buffers.empty()) {
        return;
    }
    std::vector<id<MTLResource>> resources(declared_buffers.begin(), declared_buffers.end());
    if (render) {
        // Render passes read guest memory through pointers; writes come from compute. Declaring
        // them writers too made every pass wait for the one before (no vertex/fragment overlap).
        // BB_METAL_RENDER_WRITES=1: declare writes as well.
        static const bool writes = [] {
            const char* env = std::getenv("BB_METAL_RENDER_WRITES");
            return env && env[0] == '1';
        }();
        [(id<MTLRenderCommandEncoder>)encoder
            useResources:resources.data()
                   count:resources.size()
                   usage:writes ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead
                  stages:MTLRenderStageVertex | MTLRenderStageFragment];
    } else {
        [(id<MTLComputeCommandEncoder>)encoder useResources:resources.data()
                                                      count:resources.size()
                                                      usage:MTLResourceUsageRead |
                                                            MTLResourceUsageWrite];
    }
}

// ---- Fenced ordering (BB_METAL_FENCED_ORDERING, default on, =0 to turn it off) ----
// Render encoders declared the whole guest memory (its pools) as read and compute encoders as
// read and write, and the pools are hazard-tracked: Metal then serialised every render encoder
// against the compute and blit encoders around it, dependent or not. Render encoders now declare
// nothing (the memory stays resident through the residency set) and fences keep the real
// orderings:
// - read after write: every render encoder waits, before its vertex and fragment stages, for the
//   latest compute/blit encoder (a fence each of them updates when it ends; compute and blit
//   encoders stay ordered among themselves by tracking);
// - write after read: a compute/blit encoder waits for the render encoders of its command buffer
//   before it when the renderer recorded a barrier since its last wait (the barrier tracker
//   found a conflict), and the first one of a command buffer waits for the last render encoder
//   submitted before it.
// Render encoders that write guest memory (BB_METAL_RENDER_WRITES=1) keep their declarations.
bool Fenced() {
    static const bool on = [] {
        const char* env = std::getenv("BB_METAL_FENCED_ORDERING");
        const char* writes = std::getenv("BB_METAL_RENDER_WRITES");
        return !(env && env[0] == '0') && !(writes && writes[0] == '1');
    }();
    return on && residency;
}
id<MTLFence> NonrenderFence() {
    static id<MTLFence> fence = [device newFence];
    return fence;
}
id<MTLFence> LastRenderFence() {
    static id<MTLFence> fence = [device newFence];
    return fence;
}
/// Both global fences updated once by a submitted encoder before any recorded work, so that no
/// wait ever refers to a fence nothing updated.
void SeedOrderFences() {
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    cmd.label = @"bbport fence seed";
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc updateFence:NonrenderFence()];
    [enc updateFence:LastRenderFence()];
    [enc endEncoding];
    [cmd commit];
}
constexpr MTLRenderStages kRenderStages = MTLRenderStageVertex | MTLRenderStageFragment;
void FenceRenderStart(Owned& o) {
    [o.render waitForFence:NonrenderFence() beforeStages:kRenderStages];
}
void FenceRenderEnd(Owned& o) {
    if (o.fence_next == o.fence_pool.size()) {
        o.fence_pool.push_back([device newFence]);
    }
    id<MTLFence> fence = o.fence_pool[o.fence_next++];
    [o.render updateFence:fence afterStages:kRenderStages];
    [o.render updateFence:LastRenderFence() afterStages:kRenderStages];
    o.render_fences.push_back(fence);
}
template <typename Encoder>
void FenceNonrenderStart(Owned& o, Encoder encoder) {
    if (!o.nonrender_seen) {
        [encoder waitForFence:LastRenderFence()];
        o.nonrender_seen = true;
    }
    if (o.war_pending) {
        for (id<MTLFence> fence : o.render_fences) {
            [encoder waitForFence:fence];
        }
        o.render_fences.clear();
        o.fence_next = 0; // every fence in the pool has been waited on: free again
    }
    o.war_pending = false;
}
template <typename Encoder>
void FenceNonrenderEnd(Encoder encoder) {
    [encoder updateFence:NonrenderFence()];
}

/// The render encoder of the current pass, opened on first use: a pass without draws costs
/// nothing unless it clears (EndRendering), and work before its first draw (tessellation
/// kernels) goes before it instead of splitting it.
id<MTLRenderCommandEncoder> RenderEncoder(Owned& o) {
    if (o.render || !o.pass) {
        return o.render;
    }
    if (o.compute) {
        if (Fenced()) {
            FenceNonrenderEnd(o.compute);
        }
        [o.compute endEncoding];
        o.compute = nil;
    }
    if (o.blit) {
        if (Fenced()) {
            FenceNonrenderEnd(o.blit);
        }
        [o.blit endEncoding];
        o.blit = nil;
    }
    o.render = [o.cmd renderCommandEncoderWithDescriptor:o.pass_opened ? o.pass : o.pass_first];
    o.pass_opened = true;
    o.cache = {};
    if (o.render) {
        if (Fenced()) {
            FenceRenderStart(o);
        } else {
            UseAddressBuffers(o.render, true);
        }
    }
    return o.render;
}

id<MTLComputeCommandEncoder> ComputeEncoder(Owned& o) {
    if (o.compute) {
        return o.compute;
    }
    EndEncoders(o);
    o.compute = [o.cmd computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
    UseAddressBuffers(o.compute, false);
    if (Fenced()) {
        FenceNonrenderStart(o, o.compute);
    }
    return o.compute;
}

id<MTLBlitCommandEncoder> BlitEncoder(Owned& o) {
    if (o.blit) {
        return o.blit;
    }
    EndEncoders(o);
    o.blit = [o.cmd blitCommandEncoder];
    if (Fenced()) {
        FenceNonrenderStart(o, o.blit);
    }
    return o.blit;
}

// ---- Resource binding ----

enum class Stage { Vertex, Fragment, Compute };

void SetBuffer(Owned& o, Stage stage, id<MTLBuffer> buffer, u64 offset, u32 index) {
    if (stage != Stage::Compute && index < o.cache.buffers[0].size()) {
        auto& slot = o.cache.buffers[stage == Stage::Fragment][index];
        const void* object = (__bridge void*)buffer;
        if (slot.object == object) {
            if (slot.offset == offset) {
                return;
            }
            slot.offset = offset;
            if (stage == Stage::Vertex) {
                [o.render setVertexBufferOffset:offset atIndex:index];
            } else {
                [o.render setFragmentBufferOffset:offset atIndex:index];
            }
            return;
        }
        slot = {(__bridge void*)buffer, offset};
    }
    switch (stage) {
    case Stage::Vertex:
        [o.render setVertexBuffer:buffer offset:offset atIndex:index];
        break;
    case Stage::Fragment:
        [o.render setFragmentBuffer:buffer offset:offset atIndex:index];
        break;
    case Stage::Compute:
        [o.compute setBuffer:buffer offset:offset atIndex:index];
        break;
    }
}

void SetBytes(Owned& o, Stage stage, const void* bytes, u64 length, u32 index) {
    if (stage != Stage::Compute && index < o.cache.buffers[0].size()) {
        o.cache.buffers[stage == Stage::Fragment][index] = {}; // bytes replace the buffer
    }
    switch (stage) {
    case Stage::Vertex:
        [o.render setVertexBytes:bytes length:length atIndex:index];
        break;
    case Stage::Fragment:
        [o.render setFragmentBytes:bytes length:length atIndex:index];
        break;
    case Stage::Compute:
        [o.compute setBytes:bytes length:length atIndex:index];
        break;
    }
}

void SetTexture(Owned& o, Stage stage, id<MTLTexture> texture, u32 index) {
    if (stage != Stage::Compute && index < o.cache.textures[0].size()) {
        void*& slot = o.cache.textures[stage == Stage::Fragment][index];
        if (slot == (__bridge void*)texture) {
            return;
        }
        slot = (__bridge void*)texture;
    }
    switch (stage) {
    case Stage::Vertex:
        [o.render setVertexTexture:texture atIndex:index];
        break;
    case Stage::Fragment:
        [o.render setFragmentTexture:texture atIndex:index];
        break;
    case Stage::Compute:
        [o.compute setTexture:texture atIndex:index];
        break;
    }
}

void SetSampler(Owned& o, Stage stage, id<MTLSamplerState> sampler, u32 index) {
    if (stage != Stage::Compute && index < o.cache.samplers[0].size()) {
        void*& slot = o.cache.samplers[stage == Stage::Fragment][index];
        if (slot == (__bridge void*)sampler) {
            return;
        }
        slot = (__bridge void*)sampler;
    }
    switch (stage) {
    case Stage::Vertex:
        [o.render setVertexSamplerState:sampler atIndex:index];
        break;
    case Stage::Fragment:
        [o.render setFragmentSamplerState:sampler atIndex:index];
        break;
    case Stage::Compute:
        [o.compute setSamplerState:sampler atIndex:index];
        break;
    }
}

/// Binds a stage's resources from the push descriptors (by the shader's binding map) and its
/// push constants.
void BindStage(Owned& o, Stage stage, const Metal::CompiledShader* shader,
               const std::unordered_map<u32, Binding>& bindings) {
    if (!shader) {
        return;
    }
    using Kind = Metal::ShaderBinding::Kind;
    std::array<u32, Metal::kMaxBuffers> sizes{};
    for (const auto& b : shader->bindings) {
        if (b.kind == Kind::PushConstants) {
            continue;
        }
        const auto found = bindings.find((b.set << 16) | b.binding);
        const Binding* binding = found == bindings.end() ? nullptr : &found->second;
        for (u32 e = 0; e < b.count; ++e) {
            const Bound* bound =
                binding && e < binding->elements.size() ? &binding->elements[e] : nullptr;
            const u32 slot = b.msl_index + e;
            switch (b.kind) {
            case Kind::Buffer:
                if (bound && bound->buffer) {
                    SetBuffer(o, stage, bound->buffer, bound->offset, slot);
                    if (slot < sizes.size()) {
                        sizes[slot] = u32(std::min<u64>(bound->size, 0xffffffffu));
                    }
                } else {
                    SetBuffer(o, stage, null_buffer, 0, slot);
                    if (slot < sizes.size()) {
                        sizes[slot] = u32(null_buffer.length);
                    }
                }
                break;
            case Kind::Texture:
            case Kind::StorageTexture:
            case Kind::TexelBuffer:
                SetTexture(o, stage, bound && bound->texture ? bound->texture : null_texture, slot);
                break;
            case Kind::Sampler:
                SetSampler(o, stage, bound && bound->sampler ? bound->sampler : null_sampler, slot);
                break;
            default:
                break;
            }
        }
    }
    if (shader->uses_push_constants) {
        SetBytes(o, stage, o.push.data(), o.push.size(), shader->push_constant_buffer);
    }
    if (shader->buffer_size_buffer != ~0u) {
        SetBytes(o, stage, sizes.data(), sizeof(u32) * shader->num_buffers,
                 shader->buffer_size_buffer);
    }
}

u32 IndexSize(VkIndexType type) {
    return type == VK_INDEX_TYPE_UINT32 ? 4 : 2;
}

/// Draw state from the pipeline and the dynamic state; false: skip the draw.
bool PrepareDraw(Owned& o) {
    if (!RenderEncoder(o)) {
        Log("a draw outside a render pass (skipped)");
        return false;
    }
    const auto* entry = o.graphics.get();
    if (!entry || !entry->render) {
        ++stats.skipped_no_pipeline;
        return false;
    }
    const Metal::RenderPipeline& p = *entry->render;
    if (p.primitive_emulated && !entry->rect_list && !entry->tess && !entry->geometry) {
        ++stats.skipped_emulated;
        Log("a topology Metal lacks (fan, quad list, adjacency) is not expanded yet (skipped)");
        return false;
    }
    if (o.rasterizer_discard) {
        ++stats.skipped_discard;
        return false;
    }
    // Generic (host pass) pipelines fix part of the state at creation.
    const auto& fixed = entry->fixed;
    const bool generic = entry->generic;
    const VkCullModeFlags cull_mode = generic && fixed.cull ? fixed.cull_mode : o.cull_mode;
    const VkFrontFace front_face = generic && fixed.front_face ? fixed.front : o.front_face;
    const bool depth_test = generic && fixed.depth ? fixed.depth_test : o.depth_test;
    const bool depth_write = generic && fixed.depth ? fixed.depth_write : o.depth_write;
    const VkCompareOp depth_compare = generic && fixed.depth ? fixed.depth_compare : o.depth_compare;
    const bool stencil_test = generic && fixed.stencil ? fixed.stencil_test : o.stencil_test;
    const bool depth_bias_enable =
        generic && fixed.depth_bias ? fixed.depth_bias_enable : o.depth_bias_enable;
    const auto cull = Metal::VkToMtl::CullMode(vk::CullModeFlags(cull_mode));
    if (cull.cull_all) {
        ++stats.skipped_discard;
        return false;
    }
    id<MTLRenderCommandEncoder> enc = o.render;
    auto& c = o.cache;
    if (c.pipeline != p.state) {
        c.pipeline = p.state;
        [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)p.state];
    }
    if (c.fill != p.fill_mode) {
        c.fill = p.fill_mode;
        [enc setTriangleFillMode:MTLTriangleFillMode(p.fill_mode)];
    }
    if (c.clip != u32(p.depth_clip)) {
        c.clip = u32(p.depth_clip);
        [enc setDepthClipMode:p.depth_clip ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
    }
    if (c.cull != cull.mode) {
        c.cull = cull.mode;
        [enc setCullMode:MTLCullMode(cull.mode)];
    }
    const u32 winding = Metal::RasterUtil::FrontWinding(vk::FrontFace(front_face));
    if (c.winding != winding) {
        c.winding = winding;
        [enc setFrontFacingWinding:MTLWinding(winding)];
    }

    // Viewports and scissors (Metal: negative-height viewports keep Vulkan's y; scissors clamped).
    std::array<MTLViewport, 16> viewports;
    const u32 nv = u32(std::min<size_t>(o.viewports.size(), viewports.size()));
    for (u32 i = 0; i < nv; ++i) {
        const VkViewport& v = o.viewports[i];
        const auto m = Metal::RasterUtil::ToMetalViewport(v.x, v.y, v.width, v.height, v.minDepth,
                                                          v.maxDepth);
        viewports[i] = {m.origin_x, m.origin_y, m.width, m.height, m.znear, m.zfar};
    }
    if (nv) {
        const u64 hash = XXH3_64bits(viewports.data(), sizeof(MTLViewport) * nv) ^ nv;
        if (c.viewport_hash != hash) {
            c.viewport_hash = hash;
            [enc setViewports:viewports.data() count:nv];
        }
    }
    std::array<MTLScissorRect, 16> scissors;
    const u32 ns = u32(std::min<size_t>(o.scissors.size(), scissors.size()));
    for (u32 i = 0; i < ns; ++i) {
        const VkRect2D& s = o.scissors[i];
        Metal::RasterUtil::ScissorRect r;
        if (!Metal::RasterUtil::ClampScissor(s.offset.x, s.offset.y, s.extent.width,
                                             s.extent.height, o.target_width, o.target_height,
                                             r)) {
            ++stats.skipped_discard;
            return false;
        }
        scissors[i] = {r.x, r.y, r.width, r.height};
    }
    if (ns) {
        const u64 hash = XXH3_64bits(scissors.data(), sizeof(MTLScissorRect) * ns) ^ ns;
        if (c.scissor_hash != hash) {
            c.scissor_hash = hash;
            [enc setScissorRects:scissors.data() count:ns];
        }
    }

    // Depth and stencil.
    Metal::DepthStencilKey ds;
    ds.depth_test = depth_test && p.depth_pixel_format;
    ds.depth_write = depth_write && p.depth_pixel_format;
    ds.depth_compare = u8(depth_compare);
    ds.stencil_test = stencil_test && p.stencil_pixel_format;
    // bloodborne-runtime: the current DepthStencilKey (mtl_depth_stencil.h) holds the Vulkan
    // values as bytes.
    const auto face = [](const Owned::Face& f) {
        Metal::StencilFace out;
        out.fail = u8(f.fail);
        out.pass = u8(f.pass);
        out.depth_fail = u8(f.depth_fail);
        out.compare = u8(f.compare);
        out.read_mask = u8(f.compare_mask);
        out.write_mask = u8(f.write_mask);
        return out;
    };
    ds.front = face(o.front);
    ds.back = face(o.back);
    if (void* state = MetalShadow::DepthStencil(ds); state && c.depth_stencil != state) {
        c.depth_stencil = state;
        [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)state];
    }
    if (ds.stencil_test && (c.ref_front != o.front.reference || c.ref_back != o.back.reference)) {
        c.ref_front = o.front.reference;
        c.ref_back = o.back.reference;
        [enc setStencilFrontReferenceValue:o.front.reference backReferenceValue:o.back.reference];
    }
    const float bias[3] = {depth_bias_enable ? o.depth_bias[0] : 0.0f,
                           depth_bias_enable ? o.depth_bias[1] : 0.0f,
                           depth_bias_enable ? o.depth_bias[2] : 0.0f};
    if (std::memcmp(c.bias, bias, sizeof bias) != 0) {
        std::memcpy(c.bias, bias, sizeof bias);
        [enc setDepthBias:bias[0] slopeScale:bias[2] clamp:bias[1]];
    }
    if (std::memcmp(c.blend, o.blend_constants, sizeof c.blend) != 0) {
        std::memcpy(c.blend, o.blend_constants, sizeof c.blend);
        [enc setBlendColorRed:o.blend_constants[0]
                        green:o.blend_constants[1]
                         blue:o.blend_constants[2]
                        alpha:o.blend_constants[3]];
    }

    // Resources and vertex buffers.
    BindStage(o, Stage::Vertex, p.vs, o.graphics_bindings);
    BindStage(o, Stage::Fragment, p.fs, o.graphics_bindings);
    for (u32 i = 0; i < p.num_vertex_slots; ++i) {
        const auto& slot = p.vertex_slots[i];
        const auto& vb = slot.location < o.vertex_buffers.size() ? o.vertex_buffers[slot.location]
                                                                 : Owned::VertexBuffer{};
        u64 stride = vb.stride;
        if (!stride && generic && slot.location < entry->strides.size()) {
            stride = entry->strides[slot.location];
        }
        if (slot.constant || !stride) {
            stride = 16;
        }
        [enc setVertexBuffer:vb.buffer ? vb.buffer : null_buffer
                      offset:vb.buffer ? vb.offset : 0
             attributeStride:stride
                     atIndex:slot.slot];
        if (slot.slot < c.buffers[0].size()) {
            c.buffers[0][slot.slot] = {}; // set with a stride: not a plain cached binding
        }
    }
    return true;
}

MTLPrimitiveType PrimitiveOf(const Owned& o) {
    return o.graphics->rect_list ? MTLPrimitiveTypeTriangle
                                 : MTLPrimitiveType(o.graphics->render->primitive_type);
}

/// Bytes and block size of a pixel format, for buffer <-> texture copies.
struct Block {
    u32 bytes{}, width{1}, height{1};
};
Block BlockOf(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatStencil8:
        return {1};
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint:
    case MTLPixelFormatR16Sint: case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatA1BGR5Unorm: case MTLPixelFormatABGR4Unorm:
    case MTLPixelFormatBGR5A1Unorm: case MTLPixelFormatDepth16Unorm:
        return {2};
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float:
    case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatRGBA8Sint: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG11B10Float:
    case MTLPixelFormatRGB9E5Float: case MTLPixelFormatBGR10A2Unorm: case MTLPixelFormatDepth32Float:
        return {4};
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA16Float:
        return {8};
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float:
        return {16};
    case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB: case MTLPixelFormatBC4_RUnorm:
    case MTLPixelFormatBC4_RSnorm:
        return {8, 4, 4};
    case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC2_RGBA_sRGB: case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatBC3_RGBA_sRGB: case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC5_RGSnorm:
    case MTLPixelFormatBC6H_RGBFloat: case MTLPixelFormatBC6H_RGBUfloat: case MTLPixelFormatBC7_RGBAUnorm:
    case MTLPixelFormatBC7_RGBAUnorm_sRGB:
        return {16, 4, 4};
    default:
        return {};
    }
}

void BufferImageCopy(Owned& o, BufferRef buffer, id<MTLTexture> texture,
                     const VkBufferImageCopy& r, bool to_image) {
    MTLPixelFormat format = texture.pixelFormat;
    MTLBlitOption option = MTLBlitOptionNone;
    if (r.imageSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
        option = MTLBlitOptionStencilFromDepthStencil;
        format = MTLPixelFormatStencil8;
    } else if ((r.imageSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) &&
               format == MTLPixelFormatDepth32Float_Stencil8) {
        option = MTLBlitOptionDepthFromDepthStencil;
        format = MTLPixelFormatDepth32Float;
    }
    const Block block = BlockOf(format);
    if (!block.bytes) {
        Log("a buffer/image copy with a pixel format without a size table entry (skipped)");
        return;
    }
    const u32 row_texels = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
    const u32 image_rows = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
    const u64 bytes_per_row = u64((row_texels + block.width - 1) / block.width) * block.bytes;
    const u64 bytes_per_image = bytes_per_row * ((image_rows + block.height - 1) / block.height);
    const MTLSize size = MTLSizeMake(r.imageExtent.width, r.imageExtent.height, r.imageExtent.depth);
    const MTLOrigin origin = MTLOriginMake(r.imageOffset.x, r.imageOffset.y, r.imageOffset.z);
    id<MTLBlitCommandEncoder> blit = BlitEncoder(o);
    for (u32 layer = 0; layer < r.imageSubresource.layerCount; ++layer) {
        const u64 offset = buffer.offset + r.bufferOffset + layer * bytes_per_image;
        if (to_image) {
            [blit copyFromBuffer:buffer.buffer
                       sourceOffset:offset
                  sourceBytesPerRow:bytes_per_row
                sourceBytesPerImage:bytes_per_image
                         sourceSize:size
                          toTexture:texture
                   destinationSlice:r.imageSubresource.baseArrayLayer + layer
                   destinationLevel:r.imageSubresource.mipLevel
                  destinationOrigin:origin
                            options:option];
        } else {
            [blit copyFromTexture:texture
                             sourceSlice:r.imageSubresource.baseArrayLayer + layer
                             sourceLevel:r.imageSubresource.mipLevel
                            sourceOrigin:origin
                              sourceSize:size
                                toBuffer:buffer.buffer
                       destinationOffset:offset
                  destinationBytesPerRow:bytes_per_row
                destinationBytesPerImage:bytes_per_image
                                 options:option];
        }
    }
}

} // namespace

id<MTLTexture> ImageTexture(VkImage image) {
    return TextureOfImage(image);
}

Stats& GetStats() {
    return stats;
}

VkImage ImageOfView(VkImageView view) {
    std::scoped_lock lock{maps_mutex};
    const auto it = view_infos.find(view);
    return it != view_infos.end() ? it->second.image : VK_NULL_HANDLE;
}

size_t AddressBufferCount() {
    std::scoped_lock lock{maps_mutex};
    return declared_buffers.size();
}

void Init(VkDevice dev, id<MTLCommandQueue> q, PFN_vkExportMetalObjectsEXT export_fn,
          PFN_vkGetBufferDeviceAddress address_fn) {
    vk_device = dev;
    queue = q;
    device = q.device;
    export_objects = export_fn;
    get_address = address_fn;
    get_mtl_buffer = export_fn ? FindGetMTLBuffer() : nullptr;
    if (export_fn && !get_mtl_buffer) {
        std::printf("Metal frame: vkGetMTLBufferMVK not found in MoltenVK: buffers cannot be "
                    "bound\n");
    }
    null_buffer = [device newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModePrivate;
    null_texture = [device newTextureWithDescriptor:td];
    null_sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
    rd.label = @"bbport device-address buffers";
    NSError* error = nil;
    residency = [device newResidencySetWithDescriptor:rd error:&error];
    if (residency) {
        [residency commit];
        [queue addResidencySet:residency];
        if (Fenced()) {
            SeedOrderFences();
        }
    } else {
        std::printf("Metal frame: no residency set (%s): every device-address buffer is declared "
                    "on each encoder instead\n",
                    error ? error.localizedDescription.UTF8String : "unknown");
    }
}

void EndEncoders(Owned& o) {
    FlushPending(o);
    if (o.render) {
        if (Fenced()) {
            FenceRenderEnd(o);
        }
        [o.render endEncoding];
        o.render = nil;
    }
    if (o.compute) {
        if (Fenced()) {
            FenceNonrenderEnd(o.compute);
        }
        [o.compute endEncoding];
        o.compute = nil;
    }
    if (o.blit) {
        if (Fenced()) {
            FenceNonrenderEnd(o.blit);
        }
        [o.blit endEncoding];
        o.blit = nil;
    }
}

void RetainFencesUntilDone(Owned& o) {
    if (o.fence_pool.empty()) {
        return;
    }
    // The command buffer does not retain what it uses (unretained references).
    __block std::vector<id<MTLFence>> fences = o.fence_pool;
    [o.cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
      fences.clear();
    }];
}

void ReturnScratchWhenDone(Owned& o) {
    if (o.scratch_used_buffers.empty()) {
        return;
    }
    __block std::vector<id<MTLBuffer>> buffers = std::move(o.scratch_used_buffers);
    o.scratch_used_buffers.clear();
    [o.cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
      std::scoped_lock lock{scratch_pool_mutex};
      for (id<MTLBuffer> b : buffers) {
          if (b.length == (8ull << 20) && scratch_pool.size() < 16) {
              scratch_pool.push_back(b);
          }
      }
      buffers.clear();
    }];
}

void CommitChunk(Owned& o) {
    EndEncoders(o);
    RetainFencesUntilDone(o);
    ReturnScratchWhenDone(o);
    if (!o.keep.empty()) {
        __block std::vector<id<MTLBuffer>> keep = std::move(o.keep);
        o.keep.clear();
        [o.cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
          keep.clear();
        }];
    }
    [o.cmd commit];
    o.cmd = [queue commandBufferWithUnretainedReferences];
    o.cmd.label = @"bbport Metal path";
    o.scratch = nil; // its chunk now belongs to the committed command buffer
    o.scratch_used = 0;
    o.chunk_draws = 0;
}

void SamplerCreated(VkSampler sampler, const VkSamplerCreateInfo* info) {
    using namespace Metal::VkToMtl;
    MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
    d.minFilter = MTLSamplerMinMagFilter(SamplerFilter(vk::Filter(info->minFilter)));
    d.magFilter = MTLSamplerMinMagFilter(SamplerFilter(vk::Filter(info->magFilter)));
    d.mipFilter = info->maxLod <= info->minLod && info->minLod == 0.0f
                      ? MTLSamplerMipFilterNotMipmapped
                      : MTLSamplerMipFilter(MipFilter(vk::SamplerMipmapMode(info->mipmapMode)));
    d.sAddressMode = MTLSamplerAddressMode(AddressMode(vk::SamplerAddressMode(info->addressModeU)));
    d.tAddressMode = MTLSamplerAddressMode(AddressMode(vk::SamplerAddressMode(info->addressModeV)));
    d.rAddressMode = MTLSamplerAddressMode(AddressMode(vk::SamplerAddressMode(info->addressModeW)));
    d.lodMinClamp = info->minLod;
    d.lodMaxClamp = info->maxLod;
    d.maxAnisotropy = info->anisotropyEnable ? NSUInteger(std::clamp(info->maxAnisotropy, 1.0f, 16.0f)) : 1;
    if (info->compareEnable) {
        d.compareFunction = MTLCompareFunction(SamplerCompare(vk::CompareOp(info->compareOp)));
    }
    d.borderColor = MTLSamplerBorderColor(BorderColor(vk::BorderColor(info->borderColor)).color);
    d.normalizedCoordinates = !info->unnormalizedCoordinates;
    if (info->mipLodBias != 0.0f) {
        Log("sampler LOD bias is not applied (Metal samplers have none)");
    }
    id<MTLSamplerState> state = [device newSamplerStateWithDescriptor:d];
    std::scoped_lock lock{maps_mutex};
    samplers[sampler] = state;
}

void SamplerDestroyed(VkSampler sampler) {
    std::scoped_lock lock{maps_mutex};
    samplers.erase(sampler);
}

/// 2D views of 3D images (M4): Metal has no such views, so these images stay MoltenVK's. How the
/// game uses them (drawn into, sampled, written) is logged once per use and format, to choose
/// their Metal replacement (a render pass's depth plane, or a copy of the slice).
std::unordered_set<VkImageView> slice_views;
void NoteSliceView(VkImageView view, const char* use) {
    VkFormat format{};
    u32 layer = 0;
    {
        std::scoped_lock lock{maps_mutex};
        if (!slice_views.contains(view)) {
            return;
        }
        if (auto it = view_infos.find(view); it != view_infos.end()) {
            format = it->second.format;
            layer = it->second.range.baseArrayLayer;
        }
    }
    static std::mutex logged_mutex;
    static std::set<std::pair<std::string, int>> logged;
    std::scoped_lock lock{logged_mutex};
    if (logged.emplace(use, int(format)).second) {
        std::printf("Metal frame: a 2D view of a 3D image (format %d, slice %u) is %s\n",
                    int(format), layer, use);
    }
}

void ImageViewCreated(VkImageView view, const VkImageViewCreateInfo* info) {
    const bool flat_view = info->viewType == VK_IMAGE_VIEW_TYPE_2D ||
                           info->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    id<MTLTexture> image = flat_view ? TextureOfImage(info->image) : nil;
    std::scoped_lock lock{maps_mutex};
    view_infos[view] = {info->image, info->format, info->subresourceRange};
    views.erase(view);
    {
        // Which views MoltenVK still makes (images Metal does not make), once per kind.
        static std::set<std::pair<int, int>> kinds;
        id<MTLTexture> base = flat_view ? image : TextureOfImage(info->image);
        if (base && kinds.emplace(int(info->viewType), int(base.textureType)).second) {
            std::printf("Metal frame: a MoltenVK image (texture type %d, format %d) gets a view of "
                        "type %d\n",
                        int(base.textureType), int(base.pixelFormat), int(info->viewType));
        }
    }
    if (image && image.textureType == MTLTextureType3D) {
        slice_views.insert(view);
    } else {
        slice_views.erase(view);
    }
}

void ImageViewDestroyed(VkImageView view) {
    std::scoped_lock lock{maps_mutex};
    views.erase(view);
    view_infos.erase(view);
    slice_views.erase(view);
}

void ImageMade(VkImage image, id<MTLTexture> texture) {
    if (!texture) {
        texture = ExportTexture(image, VK_NULL_HANDLE);
    }
    if (!texture) {
        return;
    }
    std::unique_lock lock{images_mutex};
    image_textures[image] = texture;
    images_generation.fetch_add(1, std::memory_order_release); // a reused handle value
}

void ImageGone(VkImage image) {
    std::unique_lock lock{images_mutex};
    if (image_textures.erase(image)) {
        images_generation.fetch_add(1, std::memory_order_release);
    }
}

bool MakeView(VkImageView view, const VkImageViewCreateInfo* info) {
    namespace VkToMtl = Metal::VkToMtl;
    id<MTLTexture> base = nil;
    {
        std::shared_lock lock{images_mutex};
        if (auto it = image_textures.find(info->image); it != image_textures.end()) {
            base = it->second;
        }
    }
    if (!base) {
        return false;
    }
    const auto format = vk::Format(info->format);
    auto pixel_format = MTLPixelFormat(VkToMtl::PixelFormat(format));
    if (pixel_format == MTLPixelFormatInvalid) {
        return false;
    }
    const VkImageSubresourceRange& range = info->subresourceRange;
    const bool depth_stencil = VkToMtl::IsDepthPixelFormat(u32(base.pixelFormat)) ||
                               VkToMtl::HasStencilPixelFormat(u32(base.pixelFormat));
    if (depth_stencil) {
        // A stencil-only view reads the stencil plane; a depth view the texture itself.
        pixel_format = base.pixelFormat;
        if (range.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT &&
            VkToMtl::IsDepthPixelFormat(u32(base.pixelFormat))) {
#if TARGET_OS_IPHONE
            pixel_format = MTLPixelFormatX32_Stencil8; // iOS: 32-bit depth only
#else
            pixel_format = base.pixelFormat == MTLPixelFormatDepth32Float_Stencil8
                               ? MTLPixelFormatX32_Stencil8
                               : MTLPixelFormatX24_Stencil8;
#endif
        }
    }
    const NSUInteger levels = range.levelCount == VK_REMAINING_MIP_LEVELS
                                  ? base.mipmapLevelCount - range.baseMipLevel
                                  : range.levelCount;
    const bool volume = base.textureType == MTLTextureType3D;
    const NSUInteger base_slices =
        base.textureType == MTLTextureTypeCube ? 6 : base.arrayLength;
    const NSUInteger layers = range.layerCount == VK_REMAINING_ARRAY_LAYERS
                                  ? base_slices - range.baseArrayLayer
                                  : range.layerCount;
    const bool multisample = base.sampleCount > 1;
    MTLTextureType type = MTLTextureType2D;
    switch (info->viewType) {
    case VK_IMAGE_VIEW_TYPE_3D:
        type = MTLTextureType3D;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
        type = MTLTextureTypeCube;
        break;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        type = MTLTextureTypeCubeArray;
        break;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
        type = multisample ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray;
        break;
    default: // 1D images are 2D textures (NewTexture)
        type = multisample ? MTLTextureType2DMultisample : MTLTextureType2D;
        break;
    }
    const NSRange level_range = NSMakeRange(range.baseMipLevel, levels);
    const NSRange slice_range = volume ? NSMakeRange(0, 1) : NSMakeRange(range.baseArrayLayer, layers);
    const auto swizzle = depth_stencil ? VkToMtl::IdentitySwizzle
                                       : VkToMtl::ComposeSwizzle(vk::ComponentMapping{
                                                                     vk::ComponentSwizzle(info->components.r),
                                                                     vk::ComponentSwizzle(info->components.g),
                                                                     vk::ComponentSwizzle(info->components.b),
                                                                     vk::ComponentSwizzle(info->components.a)},
                                                                 format);
    id<MTLTexture> texture = nil;
    if (pixel_format == base.pixelFormat && type == base.textureType && range.baseMipLevel == 0 &&
        levels == base.mipmapLevelCount && (volume || (range.baseArrayLayer == 0 && layers == base_slices)) &&
        swizzle == VkToMtl::IdentitySwizzle) {
        texture = base; // the whole texture as it is
    } else if (swizzle == VkToMtl::IdentitySwizzle) {
        texture = [base newTextureViewWithPixelFormat:pixel_format
                                          textureType:type
                                               levels:level_range
                                               slices:slice_range];
    } else {
        texture = [base newTextureViewWithPixelFormat:pixel_format
                                          textureType:type
                                               levels:level_range
                                               slices:slice_range
                                              swizzle:MTLTextureSwizzleChannelsMake(
                                                          MTLTextureSwizzle(swizzle.r),
                                                          MTLTextureSwizzle(swizzle.g),
                                                          MTLTextureSwizzle(swizzle.b),
                                                          MTLTextureSwizzle(swizzle.a))];
    }
    if (!texture) {
        return false;
    }
    std::scoped_lock lock{maps_mutex};
    views[view] = texture;
    view_infos[view] = {info->image, info->format, range};
    return true;
}

void BufferMade(VkBuffer buffer) {
    const BufferRef ref = ResolveFromDriver(buffer);
    if (!ref.buffer) {
        return;
    }
    std::unique_lock lock{registered_mutex};
    registered[buffer] = ref;
    registered_generation.fetch_add(1, std::memory_order_release); // a reused handle value
}

void MetalBufferMade(VkBuffer buffer, id<MTLBuffer> mtl) {
    std::unique_lock lock{registered_mutex};
    registered[buffer] = {mtl, 0};
    registered_generation.fetch_add(1, std::memory_order_release);
}

u64 BufferAddress(VkBuffer buffer) {
    const BufferRef ref = ResolveBuffer(buffer);
    return ref.buffer ? ref.buffer.gpuAddress + ref.offset : 0;
}

void BufferGone(VkBuffer buffer) {
    std::unique_lock lock{registered_mutex};
    if (registered.erase(buffer)) {
        registered_generation.fetch_add(1, std::memory_order_release);
    }
}

void DeviceAddressTaken(VkBuffer buffer) {
    const BufferRef ref = ResolveBuffer(buffer);
    if (!ref.buffer) {
        return;
    }
    std::scoped_lock lock{maps_mutex};
    if (std::find(address_buffers.begin(), address_buffers.end(), ref.buffer) ==
        address_buffers.end()) {
        address_buffers.push_back(ref.buffer);
        if (ref.buffer.length >= kDeclaredMinBytes || !residency) {
            declared_buffers.push_back(ref.buffer);
        }
        if (residency) {
            [residency addAllocation:ref.buffer];
            [residency commit];
        }
        ++address_generation;
    }
}

void Barrier(Owned& o) {
    o.war_pending = true; // fenced ordering: the next compute/blit encoder waits for renders
    // Tracked resources order the encoders; inside a compute encoder dispatches are serial.
    // A blit encoder is ended so the next command sees its writes.
    if (o.blit) {
        if (Fenced()) {
            FenceNonrenderEnd(o.blit);
        }
        [o.blit endEncoding];
        o.blit = nil;
    }
}

namespace {
/// The texture, level and slice to render into for an attachment view: the image's own texture
/// when it has the view's pixel format (MoltenVK's exported view textures can lack render-target
/// usage), else the view's.
struct Attachment {
    id<MTLTexture> texture;
    u32 level{}, slice{};
};
Attachment AttachmentOf(VkImageView view) {
    NoteSliceView(view, "drawn into (a render target)");
    ViewInfo vi{};
    bool known = false;
    {
        std::scoped_lock lock{maps_mutex};
        if (auto it = view_infos.find(view); it != view_infos.end()) {
            vi = it->second;
            known = true;
        }
    }
    id<MTLTexture> view_texture = TextureOfView(view);
    if (known) {
        id<MTLTexture> image = TextureOfImage(vi.image);
        if (image && (!view_texture || image.pixelFormat == view_texture.pixelFormat)) {
            return {image, vi.range.baseMipLevel, vi.range.baseArrayLayer};
        }
    }
    return {view_texture, 0, 0};
}
} // namespace

void BeginRendering(Owned& o, const VkRenderingInfo* info) {
    EndEncoders(o);
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    const auto load = [](VkAttachmentLoadOp op) {
        return op == VK_ATTACHMENT_LOAD_OP_CLEAR       ? MTLLoadActionClear
               : op == VK_ATTACHMENT_LOAD_OP_DONT_CARE ? MTLLoadActionDontCare
                                                       : MTLLoadActionLoad;
    };
    const auto store = [](VkAttachmentStoreOp op) {
        return op == VK_ATTACHMENT_STORE_OP_DONT_CARE ? MTLStoreActionDontCare
                                                      : MTLStoreActionStore;
    };
    u32 width = info->renderArea.offset.x + info->renderArea.extent.width;
    u32 height = info->renderArea.offset.y + info->renderArea.extent.height;
    bool any = false;
    for (u32 i = 0; i < info->colorAttachmentCount; ++i) {
        const VkRenderingAttachmentInfo& a = info->pColorAttachments[i];
        const Attachment at = a.imageView ? AttachmentOf(a.imageView) : Attachment{};
        id<MTLTexture> texture = at.texture;
        if (!texture) {
            continue;
        }
        any = true;
        rp.colorAttachments[i].texture = texture;
        rp.colorAttachments[i].level = at.level;
        rp.colorAttachments[i].slice = at.slice;
        rp.colorAttachments[i].loadAction = load(a.loadOp);
        rp.colorAttachments[i].storeAction = store(a.storeOp);
        const auto& c = a.clearValue.color.float32;
        rp.colorAttachments[i].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        width = u32(std::max<NSUInteger>(1, texture.width >> at.level));
        height = u32(std::max<NSUInteger>(1, texture.height >> at.level));
    }
    const VkRenderingAttachmentInfo* depth = info->pDepthAttachment;
    const VkRenderingAttachmentInfo* stencil = info->pStencilAttachment;
    if (depth && depth->imageView) {
        const Attachment at = AttachmentOf(depth->imageView);
        if (id<MTLTexture> texture = at.texture) {
            any = true;
            rp.depthAttachment.texture = texture;
            rp.depthAttachment.level = at.level;
            rp.depthAttachment.slice = at.slice;
            rp.depthAttachment.loadAction = load(depth->loadOp);
            rp.depthAttachment.storeAction = store(depth->storeOp);
            rp.depthAttachment.clearDepth = depth->clearValue.depthStencil.depth;
            width = u32(std::max<NSUInteger>(1, texture.width >> at.level));
            height = u32(std::max<NSUInteger>(1, texture.height >> at.level));
        }
    }
    if (stencil && stencil->imageView) {
        const Attachment at = AttachmentOf(stencil->imageView);
        if (id<MTLTexture> texture = at.texture) {
            any = true;
            rp.stencilAttachment.texture = texture;
            rp.stencilAttachment.level = at.level;
            rp.stencilAttachment.slice = at.slice;
            rp.stencilAttachment.loadAction = load(stencil->loadOp);
            rp.stencilAttachment.storeAction = store(stencil->storeOp);
            rp.stencilAttachment.clearStencil = stencil->clearValue.depthStencil.stencil;
        }
    }
    if (!any) {
        rp.renderTargetWidth = std::max(1u, width);
        rp.renderTargetHeight = std::max(1u, height);
        rp.defaultRasterSampleCount = 1;
    }
    if (info->layerCount > 1) {
        rp.renderTargetArrayLength = info->layerCount;
    }
    o.target_width = width;
    o.target_height = height;
    o.rendering = true;
    o.last_draw_tess = false;
    o.pass_first = rp;
    o.pass_opened = false;
    o.pass_clears = false;
    for (u32 i = 0; i < info->colorAttachmentCount; ++i) {
        o.pass_clears |= rp.colorAttachments[i].texture &&
                         rp.colorAttachments[i].loadAction == MTLLoadActionClear;
    }
    o.pass_clears |= (rp.depthAttachment.texture &&
                      rp.depthAttachment.loadAction == MTLLoadActionClear) ||
                     (rp.stencilAttachment.texture &&
                      rp.stencilAttachment.loadAction == MTLLoadActionClear);
    // A reopened pass loads what this one cleared and drew.
    MTLRenderPassDescriptor* again = [rp copy];
    for (u32 i = 0; i < info->colorAttachmentCount; ++i) {
        again.colorAttachments[i].loadAction = MTLLoadActionLoad;
        again.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    again.depthAttachment.loadAction = MTLLoadActionLoad;
    again.depthAttachment.storeAction = MTLStoreActionStore;
    again.stencilAttachment.loadAction = MTLLoadActionLoad;
    again.stencilAttachment.storeAction = MTLStoreActionStore;
    o.pass = again;
}

void EndRendering(Owned& o) {
    FlushPending(o);
    if (!o.render && !o.pass_opened && o.pass_clears) {
        RenderEncoder(o); // a pass without draws still performs its clears
    }
    if (o.render) {
        if (Fenced()) {
            FenceRenderEnd(o);
        }
        [o.render endEncoding];
        o.render = nil;
    }
    o.rendering = false;
    o.pass = nil;
    o.pass_first = nil;
    // BB_METAL_CHUNK=<draws> (default 0: off): commit partway through the frame. Off because the
    // renderer writes guest data into GPU memory from the CPU while it records (host copies),
    // assuming nothing of the frame runs before the submit: an early chunk read data meant for
    // later draws (broken geometry while walking). Needs those writes ordered first.
    static const u32 chunk = [] {
        const char* env = std::getenv("BB_METAL_CHUNK");
        return env ? u32(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    if (chunk && o.chunk_draws >= chunk) {
        CommitChunk(o);
    }
}

void BindPipeline(Owned& o, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (point == VK_PIPELINE_BIND_POINT_COMPUTE) {
        o.compute_pipeline = MetalShadow::FindCompute(pipeline);
    } else {
        o.graphics = MetalShadow::FindGraphics(pipeline);
        o.graphics_bound = true;
    }
}

namespace {
/// The Metal objects of one descriptor write, into `elements` (grown as needed).
void DecodeWrite(const VkWriteDescriptorSet& write, std::vector<Bound>& elements);
std::mutex sets_mutex;
std::unordered_map<VkDescriptorSet, std::unordered_map<u32, Binding>> descriptor_sets;
} // namespace

void DescriptorSetsUpdated(u32 count, const VkWriteDescriptorSet* writes) {
    std::scoped_lock lock{sets_mutex};
    for (u32 w = 0; w < count; ++w) {
        DecodeWrite(writes[w], descriptor_sets[writes[w].dstSet][writes[w].dstBinding].elements);
    }
}

void BindDescriptorSets(Owned& o, VkPipelineBindPoint point, u32 first_set, u32 count,
                        const VkDescriptorSet* sets) {
    auto& bindings = point == VK_PIPELINE_BIND_POINT_COMPUTE ? o.compute_bindings
                                                             : o.graphics_bindings;
    std::scoped_lock lock{sets_mutex};
    for (u32 i = 0; i < count; ++i) {
        auto it = descriptor_sets.find(sets[i]);
        if (it == descriptor_sets.end()) {
            continue;
        }
        for (const auto& [binding, contents] : it->second) {
            bindings[((first_set + i) << 16) | binding] = contents;
        }
    }
}

void PushDescriptorSet(Owned& o, VkPipelineBindPoint point, u32 set, u32 count,
                       const VkWriteDescriptorSet* writes) {
    auto& bindings = point == VK_PIPELINE_BIND_POINT_COMPUTE ? o.compute_bindings
                                                             : o.graphics_bindings;
    for (u32 w = 0; w < count; ++w) {
        DecodeWrite(writes[w], bindings[(set << 16) | writes[w].dstBinding].elements);
    }
}

namespace {
void DecodeWrite(const VkWriteDescriptorSet& write, std::vector<Bound>& elements) {
    {
        if (elements.size() < write.dstArrayElement + write.descriptorCount) {
            elements.resize(write.dstArrayElement + write.descriptorCount);
        }
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            Bound& b = elements[write.dstArrayElement + i];
            b = {};
            switch (write.descriptorType) {
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
                const VkDescriptorBufferInfo& info = write.pBufferInfo[i];
                const BufferRef ref = ResolveBuffer(info.buffer);
                b.buffer = ref.buffer;
                b.offset = ref.offset + info.offset;
                b.size = info.range == VK_WHOLE_SIZE && ref.buffer
                             ? ref.buffer.length - b.offset
                             : info.range;
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                NoteSliceView(write.pImageInfo[i].imageView,
                              write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                  ? "written or read as a storage image"
                                  : "sampled");
                b.texture = TextureOfView(write.pImageInfo[i].imageView);
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLER: {
                std::scoped_lock lock{maps_mutex};
                auto it = samplers.find(write.pImageInfo[i].sampler);
                b.sampler = it == samplers.end() ? nil : it->second;
                break;
            }
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
                NoteSliceView(write.pImageInfo[i].imageView, "sampled");
                b.texture = TextureOfView(write.pImageInfo[i].imageView);
                std::scoped_lock lock{maps_mutex};
                auto it = samplers.find(write.pImageInfo[i].sampler);
                b.sampler = it == samplers.end() ? nil : it->second;
                break;
            }
            default:
                Log("a descriptor type the Metal path does not bind yet");
                break;
            }
        }
    }
}
} // namespace

void PushConstants(Owned& o, u32 offset, u32 size, const void* values) {
    if (offset + size <= o.push.size()) {
        std::memcpy(o.push.data() + offset, values, size);
    }
}

void BindVertexBuffers(Owned& o, u32 first, u32 count, const VkBuffer* buffers,
                       const VkDeviceSize* offsets, const VkDeviceSize* strides) {
    for (u32 i = 0; i < count && first + i < o.vertex_buffers.size(); ++i) {
        const BufferRef ref = ResolveBuffer(buffers[i]);
        auto& vb = o.vertex_buffers[first + i];
        vb.buffer = ref.buffer;
        vb.offset = ref.offset + offsets[i];
        if (strides) {
            vb.stride = strides[i];
        }
    }
}

void BindIndexBuffer(Owned& o, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    const BufferRef ref = ResolveBuffer(buffer);
    o.index_buffer = ref.buffer;
    o.index_offset = ref.offset + offset;
    o.index_type = type;
    if (type == VK_INDEX_TYPE_UINT8_EXT) {
        Log("8-bit indices are not widened yet (drawn as 16-bit)");
    }
}

void SetViewports(Owned& o, u32 count, const VkViewport* viewports) {
    o.viewports.assign(viewports, viewports + count);
}

void SetScissors(Owned& o, u32 count, const VkRect2D* scissors) {
    o.scissors.assign(scissors, scissors + count);
}

namespace {
DrawSnap Snap(const Owned& o) {
    DrawSnap s;
    s.graphics = o.graphics;
    s.push = o.push;
    s.bindings = o.graphics_bindings;
    s.viewports = o.viewports;
    s.scissors = o.scissors;
    s.cull_mode = o.cull_mode;
    s.front_face = o.front_face;
    s.depth_test = o.depth_test;
    s.depth_write = o.depth_write;
    s.stencil_test = o.stencil_test;
    s.depth_bias_enable = o.depth_bias_enable;
    s.rasterizer_discard = o.rasterizer_discard;
    s.depth_compare = o.depth_compare;
    s.front = o.front;
    s.back = o.back;
    std::copy_n(o.depth_bias, 3, s.depth_bias);
    std::copy_n(o.blend_constants, 4, s.blend_constants);
    return s;
}

void Apply(Owned& o, DrawSnap&& s) {
    o.graphics = s.graphics;
    o.push = s.push;
    o.graphics_bindings = std::move(s.bindings);
    o.viewports = std::move(s.viewports);
    o.scissors = std::move(s.scissors);
    o.cull_mode = s.cull_mode;
    o.front_face = s.front_face;
    o.depth_test = s.depth_test;
    o.depth_write = s.depth_write;
    o.stencil_test = s.stencil_test;
    o.depth_bias_enable = s.depth_bias_enable;
    o.rasterizer_discard = s.rasterizer_discard;
    o.depth_compare = s.depth_compare;
    o.front = s.front;
    o.back = s.back;
    std::copy_n(s.depth_bias, 3, o.depth_bias);
    std::copy_n(s.blend_constants, 4, o.blend_constants);
}

/// Bump allocation from the command buffer's scratch buffer (16-byte aligned).
std::pair<id<MTLBuffer>, u64> Scratch(Owned& o, u64 size) {
    constexpr u64 kChunk = 8ull << 20;
    size = (size + 15) & ~u64(15);
    if (!o.scratch || o.scratch_used + size > o.scratch.length) {
        // Reused across frames: fresh GPU memory is mapped on first use (a stall).
        o.scratch = nil;
        if (size <= kChunk) {
            std::scoped_lock lock{scratch_pool_mutex};
            if (!scratch_pool.empty()) {
                o.scratch = scratch_pool.back();
                scratch_pool.pop_back();
            }
        }
        if (!o.scratch) {
            o.scratch = [device newBufferWithLength:std::max(kChunk, size)
                                            options:MTLResourceStorageModePrivate];
        }
        o.scratch_used_buffers.push_back(o.scratch);
        o.scratch_used = 0;
    }
    const u64 at = o.scratch_used;
    o.scratch_used += size;
    return {o.scratch, at};
}

/// A tessellated draw (as Huax-1839's RecordTessellationDraw): the vertex and control shaders as
/// compute kernels writing scratch memory, then the patch draw in the reopened render pass.
void TessellatedDraw(Owned& o, u32 count, u32 instance_count, u32 first, s32 vertex_offset,
                     u32 first_instance, bool indexed, u32 first_index) {
    const auto* entry = o.graphics.get();
    const Metal::TessellationPipeline& tess = *entry->tess;
    ++stats.tess_draws;
    if (o.last_draw_tess) {
        ++stats.tess_after_tess;
    }
    const u32 patches = tess.input_control_points ? count / tess.input_control_points : 0;
    if (!patches || !instance_count) {
        return;
    }
    if (instance_count > 1) {
        // One tessellated draw per instance, each with its own kernels, scratch and instance
        // number. Skipped before, whole trees went missing (about 1,900 draws per 5 s in the
        // Forbidden Woods). Adapted from Wooloomooloo2/bloodborne_mac_port 4565225 (GPL-2.0).
        // BB_TESS_INSTANCES=0 skips them again (A/B).
        static const bool skip = [] {
            const char* env = std::getenv("BB_TESS_INSTANCES");
            return env && env[0] == '0';
        }();
        if (skip) {
            ++stats.skipped_tess;
            Log("instanced tessellated draws are not encoded (skipped)");
            return;
        }
        --stats.tess_draws; // counted per instance below
        for (u32 i = 0; i < instance_count; ++i) {
            TessellatedDraw(o, count, 1, first, vertex_offset, first_instance + i, indexed, first_index);
        }
        return;
    }
    if (!o.pass) {
        Log("a tessellated draw outside a render pass (skipped)");
        return;
    }
    Metal::Tessellation::DrawParams params;
    params.num_vertices = patches * tess.input_control_points;
    params.first_vertex = indexed ? u32(vertex_offset) : first;
    params.first_instance = first_instance;
    if (indexed) {
        if (!o.index_buffer || o.index_type == VK_INDEX_TYPE_UINT8_EXT) {
            Log("a tessellated indexed draw without usable indices (skipped)");
            return;
        }
        const u32 size = IndexSize(o.index_type);
        params.index_type = size == 4 ? 2 : 1;
        params.index_buffer = (__bridge void*)o.index_buffer;
        params.index_offset = o.index_offset + u64(first_index) * size;
    }
    std::array<Metal::Tessellation::VertexBinding, 32> bindings;
    const u32 num_bindings = std::min<u32>(tess.num_vertex_slots, bindings.size());
    for (u32 j = 0; j < num_bindings; ++j) {
        const auto& slot = tess.vertex_slots[j];
        const auto& vb = slot.location < o.vertex_buffers.size() ? o.vertex_buffers[slot.location]
                                                                 : Owned::VertexBuffer{};
        bindings[j] = {(__bridge void*)(vb.buffer ? vb.buffer : null_buffer),
                       vb.buffer ? vb.offset : 0, slot.constant || !vb.stride ? 16 : vb.stride,
                       slot.slot};
    }
    // Steps 1 and 2: the kernels go before the pass when it has not begun, else interrupt it.
    id<MTLComputeCommandEncoder> enc = ComputeEncoder(o);
    const auto plan = Metal::Tessellation::PlanScratch(tess, patches);
    const auto [scratch, scratch_offset] = Scratch(o, plan.total);
    const auto bind = [&](const Metal::CompiledShader& shader) {
        BindStage(o, Stage::Compute, &shader, o.graphics_bindings);
    };
    Metal::Tessellation::EncodeKernels((__bridge void*)enc, tess, params,
                                       std::span{bindings.data(), num_bindings},
                                       (__bridge void*)scratch, scratch_offset, plan, bind, bind);
    // Step 3 waits for the end of the run of tessellated draws (FlushPending): the kernels of
    // the whole run share one compute pass and the patch draws one reopened render pass.
    PendingPatch pending;
    pending.snap = Snap(o);
    pending.scratch = scratch;
    pending.control_offset = scratch_offset + plan.control_offset;
    pending.patch_offset = scratch_offset + plan.patch_offset;
    pending.factor_offset = scratch_offset + plan.factor_offset;
    pending.patches = plan.patches;
    pending.output_control_points = tess.output_control_points;
    pending.instance = first_instance;
    o.pending.push_back(std::move(pending));
    ++stats.draws;
    o.last_draw_tess = true;
}
/// A geometry-shader draw (as Huax-1839's RecordGeometryDraw): per chunk of primitives the
/// export shader and the geometry shader as compute kernels writing vertex records and strip
/// indices, then the strip draws (pass-through vertex function) in the reopened render pass.
void GeometryDraw(Owned& o, u32 count, u32 instance_count, u32 first, s32 vertex_offset,
                  u32 first_instance, bool indexed, u32 first_index) {
    const Metal::GeometryPipeline& geo = *o.graphics->geometry;
    const u32 prims = geo.info.input_vertices ? count / geo.info.input_vertices : 0;
    if (!prims || !instance_count) {
        return;
    }
    if (instance_count > 1) {
        Log("instanced geometry-shader draws draw instance 0 only");
    }
    if (!o.pass) {
        Log("a geometry-shader draw outside a render pass (skipped)");
        return;
    }
    Metal::Tessellation::DrawParams params;
    params.num_vertices = prims * geo.info.input_vertices;
    params.first_vertex = indexed ? u32(vertex_offset) : first;
    params.first_instance = first_instance;
    u64 index_size = 0;
    if (indexed) {
        if (!o.index_buffer || o.index_type == VK_INDEX_TYPE_UINT8_EXT) {
            Log("a geometry-shader indexed draw without usable indices (skipped)");
            return;
        }
        index_size = IndexSize(o.index_type);
        params.index_type = index_size == 4 ? 2 : 1;
        params.index_buffer = (__bridge void*)o.index_buffer;
        params.index_offset = o.index_offset + u64(first_index) * index_size;
    }
    std::array<Metal::Tessellation::VertexBinding, 32> bindings;
    const u32 num_bindings = std::min<u32>(geo.num_vertex_slots, bindings.size());
    for (u32 j = 0; j < num_bindings; ++j) {
        const auto& slot = geo.vertex_slots[j];
        const auto& vb = slot.location < o.vertex_buffers.size() ? o.vertex_buffers[slot.location]
                                                                 : Owned::VertexBuffer{};
        bindings[j] = {(__bridge void*)(vb.buffer ? vb.buffer : null_buffer),
                       vb.buffer ? vb.offset : 0, slot.constant || !vb.stride ? 16 : vb.stride,
                       slot.slot};
    }
    id<MTLComputeCommandEncoder> enc = ComputeEncoder(o);
    const auto bind = [&](const Metal::CompiledShader& shader) {
        BindStage(o, Stage::Compute, &shader, o.graphics_bindings);
    };
    struct Chunk {
        Metal::Geometry::Plan plan;
        id<MTLBuffer> buffer;
        u64 offset;
    };
    std::vector<Chunk> chunks;
    const u32 chunk_prims = Metal::Geometry::MaxPrimitivesPerChunk(geo);
    for (u32 at = 0; at < prims; at += chunk_prims) {
        const u32 n = std::min(chunk_prims, prims - at);
        Chunk chunk;
        chunk.plan = Metal::Geometry::PlanScratch(geo, n);
        std::tie(chunk.buffer, chunk.offset) = Scratch(o, chunk.plan.total);
        Metal::Tessellation::DrawParams draw = params;
        draw.num_vertices = n * geo.info.input_vertices;
        const u64 first_vertex = u64(at) * geo.info.input_vertices;
        if (indexed) {
            draw.index_offset = params.index_offset + first_vertex * index_size;
        } else {
            draw.first_vertex = params.first_vertex + u32(first_vertex);
        }
        Metal::Geometry::EncodeKernels((__bridge void*)enc, geo, draw, at,
                                       std::span{bindings.data(), num_bindings},
                                       (__bridge void*)chunk.buffer, chunk.offset, chunk.plan,
                                       bind, bind);
        chunks.push_back(chunk);
    }
    if (!PrepareDraw(o)) {
        return;
    }
    for (const Chunk& chunk : chunks) {
        // The pass-through function reads the records at its buffer 0.
        SetBuffer(o, Stage::Vertex, chunk.buffer, chunk.offset + chunk.plan.record_offset, 0);
        [o.render drawIndexedPrimitives:MTLPrimitiveType(geo.render->primitive_type)
                             indexCount:u64(chunk.plan.prims) * geo.info.IndexSlots()
                              indexType:MTLIndexTypeUInt32
                            indexBuffer:chunk.buffer
                      indexBufferOffset:chunk.offset + chunk.plan.index_offset
                          instanceCount:1
                             baseVertex:0
                           baseInstance:0];
    }
    ++stats.draws;
    ++o.chunk_draws;
}
} // namespace

void FlushPending(Owned& o) {
    if (o.pending.empty()) {
        return;
    }
    std::vector<PendingPatch> pending = std::move(o.pending);
    o.pending.clear();
    DrawSnap current = Snap(o);
    for (PendingPatch& p : pending) {
        Apply(o, std::move(p.snap));
        if (!PrepareDraw(o)) {
            continue;
        }
        [o.render setTessellationFactorBuffer:p.scratch offset:p.factor_offset instanceStride:0];
        SetBuffer(o, Stage::Vertex, p.scratch, p.control_offset, Metal::kTessInputBuffer);
        SetBuffer(o, Stage::Vertex, p.scratch, p.patch_offset, Metal::kTessPatchBuffer);
        SetBuffer(o, Stage::Vertex, p.scratch, p.factor_offset, Metal::kTessFactorBuffer);
        [o.render drawPatches:p.output_control_points
                   patchStart:0
                   patchCount:p.patches
             patchIndexBuffer:nil
       patchIndexBufferOffset:0
                instanceCount:1
                 baseInstance:p.instance];
    }
    Apply(o, std::move(current));
}

void Draw(Owned& o, u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) {
    if (!(o.graphics && o.graphics->tess)) {
        o.last_draw_tess = false;
        FlushPending(o);
    }
    if (o.graphics && o.graphics->geometry) {
        GeometryDraw(o, vertex_count, instance_count, first_vertex, 0, first_instance, false, 0);
        return;
    }
    if (o.graphics && o.graphics->tess) {
        TessellatedDraw(o, vertex_count, instance_count, first_vertex, 0, first_instance, false, 0);
        return;
    }
    if (!PrepareDraw(o)) {
        return;
    }
    u32 start = first_vertex, count = vertex_count;
    if (o.graphics->rect_list) {
        // Two triangles (6 vertices) per rectangle of 3 (mtl_spirv_rectlist.h).
        start = first_vertex / 3 * 6;
        count = vertex_count / 3 * 6;
    }
    [o.render drawPrimitives:PrimitiveOf(o)
                 vertexStart:start
                 vertexCount:count
               instanceCount:instance_count
                baseInstance:first_instance];
    ++stats.draws;
    ++o.chunk_draws;
}

void DrawIndexed(Owned& o, u32 index_count, u32 instance_count, u32 first_index,
                 s32 vertex_offset, u32 first_instance) {
    if (!(o.graphics && o.graphics->tess)) {
        o.last_draw_tess = false;
        FlushPending(o);
    }
    if (o.graphics && o.graphics->geometry) {
        GeometryDraw(o, index_count, instance_count, 0, vertex_offset, first_instance, true,
                     first_index);
        return;
    }
    if (o.graphics && o.graphics->tess) {
        TessellatedDraw(o, index_count, instance_count, 0, vertex_offset, first_instance, true,
                        first_index);
        return;
    }
    if (!PrepareDraw(o)) {
        return;
    }
    if (o.graphics->rect_list) {
        Log("indexed rect list draws are not encoded (skipped)");
        return;
    }
    if (!o.index_buffer) {
        Log("an indexed draw without an index buffer (skipped)");
        return;
    }
    const u32 size = IndexSize(o.index_type);
    [o.render drawIndexedPrimitives:PrimitiveOf(o)
                         indexCount:index_count
                          indexType:size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                        indexBuffer:o.index_buffer
                  indexBufferOffset:o.index_offset + u64(first_index) * size
                      instanceCount:instance_count
                         baseVertex:vertex_offset
                       baseInstance:first_instance];
    ++stats.draws;
    ++o.chunk_draws;
}

void DrawIndirect(Owned& o, VkBuffer buffer, VkDeviceSize offset, u32 count, u32 stride,
                  bool indexed) {
    if (!PrepareDraw(o)) {
        return;
    }
    if (o.graphics->rect_list) {
        Log("indirect rect list draws are not encoded (skipped)");
        return;
    }
    const BufferRef ref = ResolveBuffer(buffer);
    if (!ref.buffer) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        const u64 at = ref.offset + offset + u64(i) * stride;
        if (indexed) {
            const u32 size = IndexSize(o.index_type);
            [o.render drawIndexedPrimitives:PrimitiveOf(o)
                                  indexType:size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                indexBuffer:o.index_buffer
                          indexBufferOffset:o.index_offset
                             indirectBuffer:ref.buffer
                       indirectBufferOffset:at];
        } else {
            [o.render drawPrimitives:PrimitiveOf(o) indirectBuffer:ref.buffer indirectBufferOffset:at];
        }
        ++stats.draws;
    }
}

void Dispatch(Owned& o, u32 x, u32 y, u32 z) {
    const Metal::ComputePipeline* p = o.compute_pipeline;
    if (!p) {
        ++stats.skipped_no_pipeline;
        return;
    }
    id<MTLComputeCommandEncoder> enc = ComputeEncoder(o);
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)p->state];
    BindStage(o, Stage::Compute, p->shader, o.compute_bindings);
    [enc dispatchThreadgroups:MTLSizeMake(x, y, z)
        threadsPerThreadgroup:MTLSizeMake(p->threads_per_group[0], p->threads_per_group[1],
                                          p->threads_per_group[2])];
    ++stats.dispatches;
}

void DispatchIndirect(Owned& o, VkBuffer buffer, VkDeviceSize offset) {
    const Metal::ComputePipeline* p = o.compute_pipeline;
    const BufferRef ref = ResolveBuffer(buffer);
    if (!p || !ref.buffer) {
        ++stats.skipped_no_pipeline;
        return;
    }
    id<MTLComputeCommandEncoder> enc = ComputeEncoder(o);
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)p->state];
    BindStage(o, Stage::Compute, p->shader, o.compute_bindings);
    [enc dispatchThreadgroupsWithIndirectBuffer:ref.buffer
                           indirectBufferOffset:ref.offset + offset
                          threadsPerThreadgroup:MTLSizeMake(p->threads_per_group[0],
                                                            p->threads_per_group[1],
                                                            p->threads_per_group[2])];
    ++stats.dispatches;
}

void CopyBuffer(Owned& o, VkBuffer src, VkBuffer dst, u32 count, const VkBufferCopy* regions) {
    const BufferRef s = ResolveBuffer(src), d = ResolveBuffer(dst);
    if (!s.buffer || !d.buffer) {
        return;
    }
    id<MTLBlitCommandEncoder> blit = BlitEncoder(o);
    for (u32 i = 0; i < count; ++i) {
        [blit copyFromBuffer:s.buffer
                 sourceOffset:s.offset + regions[i].srcOffset
                     toBuffer:d.buffer
            destinationOffset:d.offset + regions[i].dstOffset
                         size:regions[i].size];
    }
}

void FillBuffer(Owned& o, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size, u32 data) {
    const BufferRef d = ResolveBuffer(dst);
    if (!d.buffer) {
        return;
    }
    const u8 b = u8(data);
    if (data != u32(b) * 0x01010101u) {
        Log("vkCmdFillBuffer with a pattern of different bytes (filled with its low byte)");
    }
    const u64 length = size == VK_WHOLE_SIZE ? d.buffer.length - d.offset - offset : size;
    [BlitEncoder(o) fillBuffer:d.buffer range:NSMakeRange(d.offset + offset, length) value:b];
}

void UpdateBuffer(Owned& o, VkBuffer dst, VkDeviceSize offset, VkDeviceSize size,
                  const void* data) {
    const BufferRef d = ResolveBuffer(dst);
    if (!d.buffer) {
        return;
    }
    id<MTLBuffer> staging = [device newBufferWithBytes:data
                                                length:size
                                               options:MTLResourceStorageModeShared];
    o.keep.push_back(staging); // the command buffer does not retain it
    [BlitEncoder(o) copyFromBuffer:staging
                      sourceOffset:0
                          toBuffer:d.buffer
                 destinationOffset:d.offset + offset
                              size:size];
}

void CopyImage(Owned& o, VkImage src, VkImage dst, u32 count, const VkImageCopy* regions) {
    id<MTLTexture> s = TextureOfImage(src), d = TextureOfImage(dst);
    if (!s || !d) {
        return;
    }
    id<MTLBlitCommandEncoder> blit = BlitEncoder(o);
    for (u32 i = 0; i < count; ++i) {
        const VkImageCopy& r = regions[i];
        for (u32 layer = 0; layer < r.srcSubresource.layerCount; ++layer) {
            [blit copyFromTexture:s
                      sourceSlice:r.srcSubresource.baseArrayLayer + layer
                      sourceLevel:r.srcSubresource.mipLevel
                     sourceOrigin:MTLOriginMake(r.srcOffset.x, r.srcOffset.y, r.srcOffset.z)
                       sourceSize:MTLSizeMake(r.extent.width, r.extent.height, r.extent.depth)
                        toTexture:d
                 destinationSlice:r.dstSubresource.baseArrayLayer + layer
                 destinationLevel:r.dstSubresource.mipLevel
                destinationOrigin:MTLOriginMake(r.dstOffset.x, r.dstOffset.y, r.dstOffset.z)];
        }
    }
}

void CopyBufferToImage(Owned& o, VkBuffer src, VkImage dst, u32 count,
                       const VkBufferImageCopy* regions) {
    const BufferRef s = ResolveBuffer(src);
    id<MTLTexture> d = TextureOfImage(dst);
    if (!s.buffer || !d) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        BufferImageCopy(o, s, d, regions[i], true);
    }
}

void CopyImageToBuffer(Owned& o, VkImage src, VkBuffer dst, u32 count,
                       const VkBufferImageCopy* regions) {
    id<MTLTexture> s = TextureOfImage(src);
    const BufferRef d = ResolveBuffer(dst);
    if (!s || !d.buffer) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        BufferImageCopy(o, d, s, regions[i], false);
    }
}

namespace {
template <typename Each>
void ForEachSubresource(id<MTLTexture> texture, u32 count, const VkImageSubresourceRange* ranges,
                        Each&& each) {
    for (u32 r = 0; r < count; ++r) {
        const auto& range = ranges[r];
        const u32 levels = range.levelCount == VK_REMAINING_MIP_LEVELS
                               ? u32(texture.mipmapLevelCount) - range.baseMipLevel
                               : range.levelCount;
        const u32 layers = range.layerCount == VK_REMAINING_ARRAY_LAYERS
                               ? u32(texture.arrayLength) - range.baseArrayLayer
                               : range.layerCount;
        for (u32 level = 0; level < levels; ++level) {
            for (u32 layer = 0; layer < layers; ++layer) {
                each(range.aspectMask, range.baseMipLevel + level, range.baseArrayLayer + layer);
            }
        }
    }
}
} // namespace

void ClearColorImage(Owned& o, VkImage image, const VkClearColorValue* color, u32 count,
                     const VkImageSubresourceRange* ranges) {
    id<MTLTexture> texture = TextureOfImage(image);
    if (!texture) {
        return;
    }
    EndEncoders(o);
    ForEachSubresource(texture, count, ranges, [&](u32, u32 level, u32 layer) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = texture;
        rp.colorAttachments[0].level = level;
        rp.colorAttachments[0].slice = layer;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(
            color->float32[0], color->float32[1], color->float32[2], color->float32[3]);
        [[o.cmd renderCommandEncoderWithDescriptor:rp] endEncoding];
    });
}

void ClearDepthStencilImage(Owned& o, VkImage image, const VkClearDepthStencilValue* value,
                            u32 count, const VkImageSubresourceRange* ranges) {
    id<MTLTexture> texture = TextureOfImage(image);
    if (!texture) {
        return;
    }
    EndEncoders(o);
    const bool has_stencil = Metal::VkToMtl::HasStencilPixelFormat(u32(texture.pixelFormat));
    ForEachSubresource(texture, count, ranges, [&](u32 aspects, u32 level, u32 layer) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
            rp.depthAttachment.texture = texture;
            rp.depthAttachment.level = level;
            rp.depthAttachment.slice = layer;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = value->depth;
        }
        if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) && has_stencil) {
            rp.stencilAttachment.texture = texture;
            rp.stencilAttachment.level = level;
            rp.stencilAttachment.slice = layer;
            rp.stencilAttachment.loadAction = MTLLoadActionClear;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.clearStencil = value->stencil;
        }
        [[o.cmd renderCommandEncoderWithDescriptor:rp] endEncoding];
    });
}

namespace {
id<MTLLibrary> blit_library;
id<MTLSamplerState> blit_linear, blit_nearest;
std::vector<std::pair<MTLPixelFormat, id<MTLRenderPipelineState>>> blit_pipelines;
std::mutex blit_mutex;

/// Fullscreen-triangle copy of a source rectangle (normalized) into the viewport, for
/// vkCmdBlitImage: Metal's blit encoder neither scales nor filters.
id<MTLRenderPipelineState> BlitPipeline(MTLPixelFormat format) {
    std::scoped_lock lock{blit_mutex};
    for (const auto& [known, state] : blit_pipelines) {
        if (known == format) {
            return state;
        }
    }
    NSError* error = nil;
    if (!blit_library) {
        NSString* source =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "struct V { float4 pos [[position]]; float2 uv; };\n"
             "vertex V blit_vs(uint id [[vertex_id]], constant float4& src [[buffer(0)]]) {\n"
             "    float2 p = float2((id << 1) & 2, id & 2);\n"
             "    V v; v.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
             "    float2 t = float2(p.x, 1.0 - p.y);\n" // Metal NDC y up, texture y down
             "    v.uv = mix(src.xy, src.zw, t); return v; }\n"
             "fragment float4 blit_fs(V in [[stage_in]], texture2d<float> tex [[texture(0)]],\n"
             "                        sampler s [[sampler(0)]]) { return tex.sample(s, in.uv); }\n"
             "struct D { float depth [[depth(any)]]; };\n"
             "fragment D blit_depth_fs(V in [[stage_in]], depth2d<float> tex [[texture(0)]],\n"
             "                         sampler s [[sampler(0)]]) { D d; d.depth = tex.sample(s, in.uv);\n"
             "                         return d; }\n";
        blit_library = [device newLibraryWithSource:source options:nil error:&error];
        if (!blit_library) {
            std::printf("Metal frame: blit shader: %s\n", error.localizedDescription.UTF8String);
            return nil;
        }
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        blit_linear = [device newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        blit_nearest = [device newSamplerStateWithDescriptor:sd];
    }
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [blit_library newFunctionWithName:@"blit_vs"];
    // Depth images (the upscaler scales depth with vkCmdBlitImage) write depth, not color.
    const bool is_depth = Metal::VkToMtl::IsDepthPixelFormat(u32(format));
    d.fragmentFunction = [blit_library newFunctionWithName:is_depth ? @"blit_depth_fs" : @"blit_fs"];
    if (is_depth) {
        d.depthAttachmentPixelFormat = format;
        if (Metal::VkToMtl::HasStencilPixelFormat(u32(format))) {
            d.stencilAttachmentPixelFormat = format;
        }
    } else {
        d.colorAttachments[0].pixelFormat = format;
    }
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!state) {
        std::printf("Metal frame: blit pipeline for format %lu: %s\n", (unsigned long)format,
                    error.localizedDescription.UTF8String);
    }
    blit_pipelines.emplace_back(format, state);
    return state;
}
} // namespace

void BlitImage(Owned& o, VkImage src_image, VkImage dst_image, u32 count,
               const VkImageBlit* regions, VkFilter filter) {
    id<MTLTexture> src = TextureOfImage(src_image);
    id<MTLTexture> dst = TextureOfImage(dst_image);
    id<MTLRenderPipelineState> pipeline = dst ? BlitPipeline(dst.pixelFormat) : nil;
    if (!src || !dst || !pipeline) {
        return;
    }
    EndEncoders(o);
    for (u32 r = 0; r < count; ++r) {
        const VkImageBlit& b = regions[r];
        const float sw = float(std::max<NSUInteger>(1, src.width >> b.srcSubresource.mipLevel));
        const float sh = float(std::max<NSUInteger>(1, src.height >> b.srcSubresource.mipLevel));
        float uv[4] = {b.srcOffsets[0].x / sw, b.srcOffsets[0].y / sh, b.srcOffsets[1].x / sw,
                       b.srcOffsets[1].y / sh};
        // Destination rectangle; a mirrored destination swaps the source edges instead.
        int x0 = b.dstOffsets[0].x, x1 = b.dstOffsets[1].x, y0 = b.dstOffsets[0].y,
            y1 = b.dstOffsets[1].y;
        if (x1 < x0) {
            std::swap(x0, x1);
            std::swap(uv[0], uv[2]);
        }
        if (y1 < y0) {
            std::swap(y0, y1);
            std::swap(uv[1], uv[3]);
        }
        if (x1 == x0 || y1 == y0) {
            continue;
        }
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        const bool is_depth = Metal::VkToMtl::IsDepthPixelFormat(u32(dst.pixelFormat));
        auto* att = is_depth ? (MTLRenderPassAttachmentDescriptor*)rp.depthAttachment
                             : (MTLRenderPassAttachmentDescriptor*)rp.colorAttachments[0];
        att.texture = dst;
        att.level = b.dstSubresource.mipLevel;
        att.slice = b.dstSubresource.baseArrayLayer;
        att.loadAction = MTLLoadActionLoad;
        att.storeAction = MTLStoreActionStore;
        if (is_depth && Metal::VkToMtl::HasStencilPixelFormat(u32(dst.pixelFormat))) {
            rp.stencilAttachment.texture = dst;
            rp.stencilAttachment.level = b.dstSubresource.mipLevel;
            rp.stencilAttachment.slice = b.dstSubresource.baseArrayLayer;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> enc = [o.cmd renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pipeline];
        if (is_depth) {
            static id<MTLDepthStencilState> write_always = [] {
                MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
                dd.depthCompareFunction = MTLCompareFunctionAlways;
                dd.depthWriteEnabled = YES;
                return [device newDepthStencilStateWithDescriptor:dd];
            }();
            [enc setDepthStencilState:write_always];
            filter = VK_FILTER_NEAREST; // depth is not filtered
        }
        [enc setViewport:(MTLViewport){double(x0), double(y0), double(x1 - x0), double(y1 - y0),
                                       0.0, 1.0}];
        [enc setVertexBytes:uv length:sizeof uv atIndex:0];
        [enc setFragmentTexture:src atIndex:0];
        [enc setFragmentSamplerState:filter == VK_FILTER_NEAREST ? blit_nearest : blit_linear
                             atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }
}

} // namespace MetalFrame
