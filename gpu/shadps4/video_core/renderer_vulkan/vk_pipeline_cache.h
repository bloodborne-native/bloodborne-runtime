// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        VideoCore::Backend::ShaderHandle module; ///< the render adapter's
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< bbport: permutation of the previous lookup, compared first
    /// bbport: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(VideoCore::Backend::ShaderHandle module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(VideoCore::Backend::ShaderHandle module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// bbport: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<VideoCore::Backend::ShaderHandle, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
    /// bbport: an indirect draw (its vertex count is on the GPU: quad lists stay tessellated).
    bool indirect = false;
    bool allow_fixed_quad = true; ///< false when CPU vertex expansion cannot safely read a draw
};

/// bbport: quad lists drawn as indexed triangle lists (QuadsAsTriangles) instead of through the
/// tessellation emulation. On MoltenVK every tessellated draw runs its vertex stages in a compute
/// pass first, which ends the render pass around each draw (particles: ~150 per frame).
/// BB_QUADS_AS_TRIANGLES=1 (off by default: not exercised by the game yet; its tessellated
/// draws are rect lists).
bool QuadsAsTriangles();
/// bbport: rect lists whose vertex shader passes 32-bit float attributes through drawn as triangle
/// lists, two triangles per rect, the 4th corner computed on the CPU (Rasterizer::ExpandRects)
/// instead of by a tessellation stage (a compute pass per draw on MoltenVK).
/// BB_RECTS_AS_TRIANGLES=0 tessellates them all.
bool RectsAsTriangles();
/// bbport: the components of a vertex attribute format ExpandRects computes corners in (32-bit
/// floats), else 0.
u32 RectAttributeComponents(VideoCore::Backend::Format format);
/// Components in a fetch format the CPU fixed-quad expander can reproduce. Check the guest
/// encoding before SurfaceFormat can assert on an unsupported format.
u32 FixedQuadCpuComponents(AmdGpu::DataFormat data, AmdGpu::NumberFormat number);

/// bbport: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
        bool from_state = false; ///< N2.3: the flat buffer from the engine's draw state
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
};

struct PreparedDraw;

class PipelineCompiler;

class PipelineCache {
public:
    explicit PipelineCache(VideoCore::Backend::Device& device, AmdGpu::Liverpool* liverpool,
                           u32 sparse_page_shift);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    /// `indirect`: the draw's arguments are in memory (bbport: never skipped while compiling).
    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr,
                                                bool indirect = false,
                                                bool allow_fixed_quad = true);

    /// bbport: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// bbport: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// bbport: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, VideoCore::Backend::ShaderHandle,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<VideoCore::Backend::ShaderHandle> ReplaceShader(
        VideoCore::Backend::ShaderHandle module, std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    VideoCore::Backend::ShaderHandle CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding, bool cached);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    VideoCore::Backend::Device& device;
    AmdGpu::Liverpool* liverpool;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    tsl::robin_map<size_t, VideoCore::Backend::ShaderHandle> fixed_quad_modules; ///< kept
    /// bbport: per program, the first permutation index with no files in the shader cache
    /// (WarmUp reads it from the cached metadata). Cached binaries are named by program and
    /// index, and pipeline keys refer to them that way: a new permutation must never take an
    /// index a cached one has, or every key naming the old one loads the new shader (issue 7 of
    /// bloodborne-mac: black walls until the cache was deleted).
    tsl::robin_map<u64, size_t> cached_permutations;
    size_t NewPermutationIndex(u64 program_hash, size_t in_memory, bool cached);

    /// bbport BB_SHADER_AHEAD=1: a worker met a shader the GPU thread has not made yet. Translates
    /// it on the ahead thread and makes it, so its library compiles before the GPU thread needs
    /// it; the GPU thread finds the same SPIR-V's module. `binding` moves on past this stage.
    void TranslateAhead(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                        const Shader::ShaderParams& params, Shader::RuntimeInfo runtime_info,
                        Shader::Backend::Bindings& binding);
    Shader::Pools ahead_pools;
    tsl::robin_map<u64, Shader::Backend::Bindings> ahead_seen; ///< on the ahead thread

public:
    /// A game shader made through the render adapter (docs/RENDER_ADAPTER.md, A5); fatal when
    /// the driver rejects it.
    VideoCore::Backend::ShaderHandle MakeShader(std::span<const u32> spirv,
                                                std::string_view name);
    /// Releases a shader made by MakeShader (pipelines made with it keep it).
    void ReleaseShader(VideoCore::Backend::ShaderHandle module);
    /// The backend module of `module`, for the shader collector.
    vk::ShaderModule NativeModule(VideoCore::Backend::ShaderHandle module);

private:
    /// bbport: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<VideoCore::Backend::ShaderHandle,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    /// bbport BB_ASYNC_PIPELINES: a new graphics pipeline of a pass drawn every frame is compiled
    /// on worker threads (PipelineCompiler) while its draws go without it for a frame or two,
    /// instead of the game standing still for the driver (18 ms a pipeline on a GTX 1060, 1-4 s
    /// hitches on a first session). Other passes (one-time renders) compile at once, as before.
    struct PendingPipeline;
    bool AsyncSkippable(const PipelineSelection& sel, bool indirect);
    const GraphicsPipeline* FinishPipeline(const GraphicsPipelineKey& key, PendingPipeline& job);
    tsl::robin_map<GraphicsPipelineKey, std::shared_ptr<PendingPipeline>> pending_graphics;
    u64 async_started = 0, async_skipped = 0, async_waited = 0; ///< statistics
    std::unique_ptr<PipelineCompiler> compiler; ///< last: its threads are joined first
};

} // namespace Vulkan
