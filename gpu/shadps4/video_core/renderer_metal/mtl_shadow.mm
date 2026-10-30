// SPDX-License-Identifier: GPL-2.0-or-later
// From Wooloomooloo2/bloodborne_mac_port (78b59f5, GPL-2.0-or-later), adapted to bloodborne-runtime.
// bbport: the Metal shadow check (video_core/renderer_vulkan/vk_metal_shadow.h). Every pipeline the
// Vulkan path creates is built again as a Metal pipeline state from the same key and SPIR-V; the
// counts show how much of the game the Metal back end's pipelines cover. BB_METAL_SHADOW=1.

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/path_util.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_metal/mtl_depth_stencil.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_pipeline_build.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_shadow.h"
#include "video_core/renderer_metal/mtl_vk_layer.h"
#include "video_core/renderer_metal/mtl_spirv_rectlist.h"
#include "video_core/renderer_metal/mtl_geometry.h"
#include "video_core/renderer_metal/mtl_spirv_geometry.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include <xxhash.h>
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_metal_shadow.h"

namespace MetalShadow {
namespace {

// bloodborne-runtime: the game's pipelines go through Metal::PipelineFactory (mtl_pipeline_build.h,
// the fork's later builder) instead of this file's own builder; the generic pipelines below keep
// it as it was.
struct State {
    /// MoltenVK's device and queue when the Metal path is on (its pipelines must be that device's).
    Metal::Device device{MetalLayer::MtlDevice(), MetalLayer::MtlQueue()};
    std::filesystem::path dir;
    Metal::PipelineFactory factory;
    Metal::ShaderCompiler& compiler;
    Metal::PsoCache& pso;
    Metal::DepthStencilCache depth_stencil{device};
    std::mutex mutex;
    std::unordered_map<u64, std::shared_ptr<std::vector<u32>>> spirv;      ///< by VkShaderModule
    std::unordered_map<u64, std::shared_ptr<Metal::ShaderModule>> modules; ///< by VkShaderModule
    /// Metal PSOs hold compiled shader pointers beyond a Vulkan module handle's lifetime.
    /// Keep one module per distinct SPIR-V, even when Vulkan reuses a handle.
    std::unordered_map<u64, std::vector<std::shared_ptr<Metal::ShaderModule>>> module_cache;

    std::atomic<u32> graphics_ok{}, graphics_failed{}, compute_ok{}, compute_failed{};
    std::atomic<u32> rect_lists{}, tessellated{}, geometry{}, no_spirv{};
    std::atomic<u32> seen{};

    std::mutex pipelines_mutex;
    std::unordered_map<VkPipeline, std::shared_ptr<const GraphicsEntry>> graphics;
    std::unordered_map<VkPipeline, const Metal::ComputePipeline*> compute;

    static std::filesystem::path Dir() {
        auto dir = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "metal-shadow";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    State()
        : dir{Dir()}, factory{device, dir}, compiler{factory.Compiler()}, pso{factory.Psos()} {
        std::printf("Metal shadow: building every pipeline as a Metal pipeline state too (%s, %s)\n",
                    device.Name().c_str(), dir.string().c_str());
    }

    const Metal::ShaderModule* Module(vk::ShaderModule module) {
        std::scoped_lock lock{mutex};
        auto it = modules.find(u64(VkShaderModule(module)));
        if (it == modules.end()) {
            ++no_spirv;
            return nullptr;
        }
        return it->second.get();
    }

    /// The totals, at the first pipeline and every 50; each failure is explained (first 50).
    void Report(bool failure = false, const std::string& why = {}) {
        if (failure) {
            static std::atomic<u32> logged{0};
            if (logged++ < 50) {
                std::printf("Metal shadow: pipeline not built: %s\n", why.c_str());
            }
        }
        const u32 n = ++seen;
        if (n != 1 && n % 50 != 0 && !failure) {
            return;
        }
        std::printf("Metal shadow: %u pipelines: graphics %u ok / %u failed (%u rect lists, %u "
                    "tessellated, %u geometry), compute %u ok / %u failed, no SPIR-V %u\n",
                    n, graphics_ok.load(), graphics_failed.load(), rect_lists.load(),
                    tessellated.load(), geometry.load(), compute_ok.load(), compute_failed.load(),
                    no_spirv.load());
        std::fflush(stdout);
    }
};

State& Get() {
    static State state;
    return state;
}

} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* shadow = std::getenv("BB_METAL_SHADOW");
        const char* frame = std::getenv("BB_METAL_FRAME");
        return (shadow && shadow[0] == '1') || (frame && frame[0] == '1');
    }();
    return enabled;
}

std::shared_ptr<const GraphicsEntry> FindGraphics(VkPipeline pipeline) {
    auto& s = Get();
    std::scoped_lock lock{s.pipelines_mutex};
    auto it = s.graphics.find(pipeline);
    return it == s.graphics.end() ? nullptr : it->second;
}

const Metal::ComputePipeline* FindCompute(VkPipeline pipeline) {
    auto& s = Get();
    std::scoped_lock lock{s.pipelines_mutex};
    auto it = s.compute.find(pipeline);
    return it == s.compute.end() ? nullptr : it->second;
}

void* DepthStencil(const Metal::DepthStencilKey& key) {
    return Get().depth_stencil.Get(key);
}

void Forget(VkPipeline pipeline) {
    if (!Enabled()) {
        return;
    }
    auto& s = Get();
    std::scoped_lock lock{s.pipelines_mutex};
    s.graphics.erase(pipeline);
    s.compute.erase(pipeline);
}

void RegisterModule(vk::ShaderModule module, std::span<const u32> spirv) {
    auto& s = Get();
    const u64 hash = XXH3_64bits(spirv.data(), spirv.size_bytes());
    auto words = std::make_shared<std::vector<u32>>(spirv.begin(), spirv.end());
    std::scoped_lock lock{s.mutex};
    const u64 handle = u64(VkShaderModule(module));
    s.spirv[handle] = std::move(words);
    auto& known = s.module_cache[hash];
    for (const auto& candidate : known) {
        const auto old = candidate->Spirv();
        if (old.size() == spirv.size() && std::equal(old.begin(), old.end(), spirv.begin())) {
            s.modules[handle] = candidate;
            return;
        }
    }
    // Named by its SPIR-V: the MSL disk cache keys on this. The content cache also keeps its
    // compiled variants alive after the Vulkan handle is destroyed or reused.
    auto metal = std::make_shared<Metal::ShaderModule>(
        std::vector<u32>(spirv.begin(), spirv.end()), hash);
    known.push_back(metal);
    s.factory.Prefetch(metal);
    s.modules[handle] = std::move(metal);
}

void ForgetModule(vk::ShaderModule module) {
    auto& s = Get();
    const u64 handle = u64(VkShaderModule(module));
    std::scoped_lock lock{s.mutex};
    s.spirv.erase(handle);
    s.modules.erase(handle);
}

void Graphics(const Vulkan::GraphicsPipelineKey& key, std::span<const vk::ShaderModule> modules,
              std::span<const bool> has_stage, std::span<const VertexInput> inputs,
              bool aux_fragment, vk::Pipeline vk_pipeline) {
    @autoreleasepool { // pipeline-creation threads drain no pool of their own
    auto& s = Get();
    std::array<const Metal::ShaderModule*, Vulkan::MaxShaderStages> stages{};
    for (u32 i = 0; i < Vulkan::MaxShaderStages && i < has_stage.size(); ++i) {
        if (has_stage[i]) {
            stages[i] = s.Module(modules[i]);
            if (!stages[i]) {
                ++s.graphics_failed;
                s.Report(true, "a stage's SPIR-V was not seen");
                return;
            }
        }
    }
    std::vector<Metal::VertexAttribute> attributes;
    for (const auto& in : inputs) {
        attributes.push_back({in.location, in.per_instance, in.divisor});
    }
    const Metal::GraphicsPsos psos = s.factory.BuildGraphics(key, stages, attributes, aux_fragment);
    if (!psos.render) {
        ++s.graphics_failed;
        s.Report(true, psos.error);
        return;
    }
    ++s.graphics_ok;
    s.rect_lists += psos.rect_list ? 1 : 0;
    s.tessellated += psos.tessellation ? 1 : 0;
    s.geometry += psos.geometry ? 1 : 0;
    {
        std::scoped_lock lock{s.pipelines_mutex};
        s.graphics[VkPipeline(vk_pipeline)] = std::make_shared<GraphicsEntry>(GraphicsEntry{
            .render = psos.render, .tess = psos.tessellation, .geometry = psos.geometry,
            .rect_list = psos.rect_list});
    }
    s.Report();
    }
}

void Compute(const Vulkan::ComputePipelineKey& key, vk::ShaderModule module,
             vk::Pipeline vk_pipeline) {
    @autoreleasepool { // pipeline-creation threads drain no pool of their own
    auto& s = Get();
    const Metal::ShaderModule* metal = s.Module(module);
    std::string why = "its SPIR-V was not seen";
    const Metal::ComputePipeline* pipeline = metal ? s.factory.BuildCompute(key, *metal, why) : nullptr;
    if (!pipeline) {
        ++s.compute_failed;
        s.Report(true, why);
        return;
    }
    {
        std::scoped_lock lock{s.pipelines_mutex};
        s.compute[VkPipeline(vk_pipeline)] = pipeline;
    }
    ++s.compute_ok;
    s.Report();
    }
}

// ---- Generic pipelines (host passes): built from their Vulkan create infos ----

namespace {
thread_local int game_build_depth = 0;

/// Fixes the SPIR-V specialization constants at the values given (or their defaults) and turns
/// them into plain constants, so SPIRV-Cross emits no Metal function constants (a pipeline state
/// would then need a specialized function): OpSpecConstant's literal, OpSpecConstantTrue/False by
/// opcode, the SpecId decorations dropped. OpSpecConstantOp stays (none in the host shaders).
std::vector<u32> Specialize(const std::vector<u32>& words, const VkSpecializationInfo* info) {
    if (words.size() < 5) {
        return words;
    }
    std::unordered_map<u32, u32> spec_ids; // result id -> SpecId
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count) {
            break;
        }
        if (op == 71 && count >= 4 && words[i + 2] == 1) { // OpDecorate id SpecId n
            spec_ids[words[i + 1]] = words[i + 3];
        }
        i += count;
    }
    std::vector<u32> out(words.begin(), words.begin() + 5);
    out.reserve(words.size());
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16, op = words[i] & 0xffff;
        if (!count || i + count > words.size()) {
            break;
        }
        if (op == 71 && count >= 4 && words[i + 2] == 1) {
            i += count; // SpecId on what is now a plain constant
            continue;
        }
        const size_t at = out.size();
        out.insert(out.end(), words.begin() + i, words.begin() + i + count);
        if ((op == 48 || op == 49 || op == 50 || op == 51) && count >= 3) {
            u32 new_op = op == 48 ? 41u : op == 49 ? 42u : op == 50 ? 43u : 44u;
            auto it = spec_ids.find(words[i + 2]);
            if (info && it != spec_ids.end()) {
                for (u32 e = 0; e < info->mapEntryCount; ++e) {
                    const auto& entry = info->pMapEntries[e];
                    if (entry.constantID != it->second ||
                        entry.offset + entry.size > info->dataSize) {
                        continue;
                    }
                    const u8* data = static_cast<const u8*>(info->pData) + entry.offset;
                    if (op == 50) {
                        for (u32 w = 0; w < count - 3 && w * 4 < entry.size; ++w) {
                            u32 v = 0;
                            std::memcpy(&v, data + w * 4, std::min<size_t>(4, entry.size - w * 4));
                            out[at + 3 + w] = v;
                        }
                    } else if (op == 48 || op == 49) {
                        bool value = false;
                        for (size_t b = 0; b < entry.size; ++b) {
                            value |= data[b] != 0;
                        }
                        new_op = value ? 41u : 42u;
                    }
                }
            }
            out[at] = (count << 16) | new_op;
        }
        i += count;
    }
    return out;
}

std::mutex generic_mutex;
std::vector<std::unique_ptr<Metal::CompiledShader>> generic_shaders;
std::vector<std::unique_ptr<Metal::RenderPipeline>> generic_pipelines;
std::atomic<u32> generic_ok{0}, generic_failed{0};

const Metal::CompiledShader* CompileStage(State& s, const VkPipelineShaderStageCreateInfo& stage,
                                          const char* what) {
    std::shared_ptr<std::vector<u32>> words;
    {
        std::scoped_lock lock{s.mutex};
        auto it = s.spirv.find(u64(stage.module));
        if (it == s.spirv.end()) {
            return nullptr;
        }
        words = it->second;
    }
    const std::vector<u32> spec = Specialize(*words, stage.pSpecializationInfo);
    const u64 hash = XXH3_64bits(spec.data(), spec.size() * 4);
    auto result = s.compiler.Compile(spec, hash, what);
    if (result.status != Metal::CompileStatus::Ok) {
        return nullptr;
    }
    std::scoped_lock lock{generic_mutex};
    return generic_shaders.emplace_back(std::move(result.shader)).get();
}

void NoteGeneric(bool ok, const char* why) {
    ++(ok ? generic_ok : generic_failed);
    if (!ok) {
        std::printf("Metal shadow: generic pipeline not built: %s\n", why);
    }
}
} // namespace

GameBuildScope::GameBuildScope() {
    ++game_build_depth;
}
GameBuildScope::~GameBuildScope() {
    --game_build_depth;
}

void ModuleCreated(VkShaderModule module, const VkShaderModuleCreateInfo* info) {
    RegisterModule(vk::ShaderModule(module),
                   std::span<const u32>(info->pCode, info->codeSize / sizeof(u32)));
}

void ComputeCreated(const VkComputePipelineCreateInfo* info, VkPipeline pipeline) {
    if (game_build_depth || !pipeline) {
        return;
    }
    auto& s = Get();
    const auto* cs = CompileStage(s, info->stage, "host compute");
    if (!cs) {
        NoteGeneric(false, "compute shader did not translate");
        return;
    }
    Vulkan::ComputePipelineKey key{};
    key.value = size_t(XXH3_64bits(&cs, sizeof cs));
    const Metal::ComputePipeline* p = s.pso.GetComputePipeline(key, cs);
    NoteGeneric(p != nullptr, "compute pipeline state");
    if (p) {
        std::scoped_lock lock{s.pipelines_mutex};
        s.compute[pipeline] = p;
    }
}

void GraphicsCreated(const VkGraphicsPipelineCreateInfo* info, VkPipeline pipeline) {
    if (game_build_depth || !pipeline) {
        return;
    }
    using namespace Metal::VkToMtl;
    auto& s = Get();
    const Metal::CompiledShader* vs = nullptr;
    const Metal::CompiledShader* fs = nullptr;
    for (u32 i = 0; i < info->stageCount; ++i) {
        const auto& stage = info->pStages[i];
        if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
            vs = CompileStage(s, stage, "host vertex");
        } else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            fs = CompileStage(s, stage, "host fragment");
        } else {
            NoteGeneric(false, "a stage other than vertex and fragment");
            return;
        }
    }
    if (!vs) {
        NoteGeneric(false, "vertex shader did not translate");
        return;
    }
    auto entry = std::make_unique<Metal::RenderPipeline>();
    GraphicsEntry ge;
    ge.generic = true;
    @autoreleasepool {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = (__bridge id<MTLFunction>)vs->function;
        if (fs) {
            d.fragmentFunction = (__bridge id<MTLFunction>)fs->function;
        }
        // Attachments (dynamic rendering).
        const VkPipelineRenderingCreateInfo* rendering = nullptr;
        for (auto* n = static_cast<const VkBaseInStructure*>(info->pNext); n; n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                rendering = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(n);
            }
        }
        const auto* blend = info->pColorBlendState;
        if (rendering) {
            for (u32 i = 0; i < rendering->colorAttachmentCount; ++i) {
                const u32 format = PixelFormat(vk::Format(rendering->pColorAttachmentFormats[i]));
                if (format && IsDepthPixelFormat(format)) {
                    // Metal asserts on a depth format as a color target (never valid Vulkan
                    // either); the pipeline is left without a Metal counterpart.
                    const std::string why =
                        "color attachment " + std::to_string(i) + " has the depth format " +
                        vk::to_string(vk::Format(rendering->pColorAttachmentFormats[i]));
                    NoteGeneric(false, why.c_str());
                    return;
                }
                entry->color_pixel_formats[i] = format;
                d.colorAttachments[i].pixelFormat = MTLPixelFormat(format);
                if (blend && i < blend->attachmentCount) {
                    const auto& a = blend->pAttachments[i];
                    d.colorAttachments[i].writeMask =
                        MTLColorWriteMask(ColorWriteMask(vk::ColorComponentFlags(a.colorWriteMask)));
                    d.colorAttachments[i].blendingEnabled = a.blendEnable;
                    if (a.blendEnable) {
                        d.colorAttachments[i].sourceRGBBlendFactor =
                            MTLBlendFactor(BlendFactor(vk::BlendFactor(a.srcColorBlendFactor)));
                        d.colorAttachments[i].destinationRGBBlendFactor =
                            MTLBlendFactor(BlendFactor(vk::BlendFactor(a.dstColorBlendFactor)));
                        d.colorAttachments[i].rgbBlendOperation =
                            MTLBlendOperation(BlendOp(vk::BlendOp(a.colorBlendOp)));
                        d.colorAttachments[i].sourceAlphaBlendFactor =
                            MTLBlendFactor(BlendFactor(vk::BlendFactor(a.srcAlphaBlendFactor)));
                        d.colorAttachments[i].destinationAlphaBlendFactor =
                            MTLBlendFactor(BlendFactor(vk::BlendFactor(a.dstAlphaBlendFactor)));
                        d.colorAttachments[i].alphaBlendOperation =
                            MTLBlendOperation(BlendOp(vk::BlendOp(a.alphaBlendOp)));
                    }
                }
            }
            entry->num_color_attachments = rendering->colorAttachmentCount;
            if (rendering->depthAttachmentFormat) {
                entry->depth_pixel_format = PixelFormat(vk::Format(rendering->depthAttachmentFormat));
                d.depthAttachmentPixelFormat = MTLPixelFormat(entry->depth_pixel_format);
            }
            if (rendering->stencilAttachmentFormat) {
                entry->stencil_pixel_format =
                    PixelFormat(vk::Format(rendering->stencilAttachmentFormat));
                d.stencilAttachmentPixelFormat = MTLPixelFormat(entry->stencil_pixel_format);
            }
        }
        if (info->pMultisampleState) {
            d.rasterSampleCount = info->pMultisampleState->rasterizationSamples;
            entry->sample_count = info->pMultisampleState->rasterizationSamples;
        }
        // Vertex input: binding b at buffer slot kVertexBufferBase - b, dynamic stride.
        if (const auto* vi = info->pVertexInputState; vi && vi->vertexAttributeDescriptionCount) {
            MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
            for (u32 i = 0; i < vi->vertexAttributeDescriptionCount; ++i) {
                const auto& a = vi->pVertexAttributeDescriptions[i];
                vd.attributes[a.location].format = MTLVertexFormat(VertexFormat(vk::Format(a.format)));
                vd.attributes[a.location].offset = a.offset;
                vd.attributes[a.location].bufferIndex = Metal::kVertexBufferBase - a.binding;
            }
            for (u32 i = 0; i < vi->vertexBindingDescriptionCount; ++i) {
                const auto& b = vi->pVertexBindingDescriptions[i];
                const u32 slot = Metal::kVertexBufferBase - b.binding;
                vd.layouts[slot].stride = MTLBufferLayoutStrideDynamic;
                vd.layouts[slot].stepFunction = b.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE
                                                    ? MTLVertexStepFunctionPerInstance
                                                    : MTLVertexStepFunctionPerVertex;
                if (b.binding < ge.strides.size()) {
                    ge.strides[b.binding] = b.stride;
                }
                auto& vslot = entry->vertex_slots[entry->num_vertex_slots++];
                vslot.location = b.binding;
                vslot.slot = slot;
            }
            d.vertexDescriptor = vd;
        }
        const auto topology = info->pInputAssemblyState
                                  ? PrimitiveType(vk::PrimitiveTopology(info->pInputAssemblyState->topology))
                                  : PrimitiveType(vk::PrimitiveTopology::eTriangleList);
        entry->primitive_type = topology.type;
        entry->primitive_emulated = topology.needs_emulation;
        d.inputPrimitiveTopology = topology.type == MTLPrimitiveTypePoint ? MTLPrimitiveTopologyClassPoint
                                   : topology.type == MTLPrimitiveTypeLine ||
                                           topology.type == MTLPrimitiveTypeLineStrip
                                       ? MTLPrimitiveTopologyClassLine
                                       : MTLPrimitiveTopologyClassTriangle;
        NSError* error = nil;
        id<MTLRenderPipelineState> state =
            [(__bridge id<MTLDevice>)MetalLayer::MtlDevice() newRenderPipelineStateWithDescriptor:d
                                                                                           error:&error];
        if (!state) {
            std::string why = "Metal: ";
            why += error ? error.localizedDescription.UTF8String : "unknown";
            NoteGeneric(false, why.c_str());
            return;
        }
        entry->state = (__bridge_retained void*)state;
    }
    entry->vs = vs;
    entry->fs = fs;
    entry->fill_mode = MTLTriangleFillModeFill;
    entry->depth_clip = true;
    // The state Vulkan fixes unless it is dynamic.
    std::vector<VkDynamicState> dynamic;
    if (info->pDynamicState) {
        dynamic.assign(info->pDynamicState->pDynamicStates,
                       info->pDynamicState->pDynamicStates + info->pDynamicState->dynamicStateCount);
    }
    const auto is_dynamic = [&](VkDynamicState d) {
        return std::find(dynamic.begin(), dynamic.end(), d) != dynamic.end();
    };
    if (const auto* r = info->pRasterizationState) {
        ge.fixed.cull = !is_dynamic(VK_DYNAMIC_STATE_CULL_MODE);
        ge.fixed.cull_mode = r->cullMode;
        ge.fixed.front_face = !is_dynamic(VK_DYNAMIC_STATE_FRONT_FACE);
        ge.fixed.front = r->frontFace;
        ge.fixed.depth_bias = !is_dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE);
        ge.fixed.depth_bias_enable = r->depthBiasEnable;
        entry->depth_clip = !r->depthClampEnable;
    }
    if (const auto* ds = info->pDepthStencilState) {
        ge.fixed.depth = !is_dynamic(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE);
        ge.fixed.depth_test = ds->depthTestEnable;
        ge.fixed.depth_write = ds->depthWriteEnable;
        ge.fixed.depth_compare = ds->depthCompareOp;
        ge.fixed.stencil = !is_dynamic(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE);
        ge.fixed.stencil_test = ds->stencilTestEnable;
    } else {
        ge.fixed.depth = ge.fixed.stencil = true; // no depth/stencil state: both off
    }
    ge.render = entry.get();
    {
        std::scoped_lock lock{generic_mutex};
        generic_pipelines.push_back(std::move(entry));
    }
    std::scoped_lock lock{s.pipelines_mutex};
    s.graphics[pipeline] = std::make_shared<GraphicsEntry>(std::move(ge));
    NoteGeneric(true, "");
}

} // namespace MetalShadow
