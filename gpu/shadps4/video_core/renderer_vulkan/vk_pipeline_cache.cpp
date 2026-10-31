// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <pthread.h>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_set>

#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_backend_device.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "bbport_guest_memory.h"
#include "bbport_render_boundary.h"
#include "bbport_settings.h"
#include "game_profile.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_metal_shadow.h"

extern thread_local sigjmp_buf* bb_assert_recover;

#include "video_core/renderer_vulkan/vk_presenter.h"
extern std::atomic<unsigned> bb_list_shaders_done, bb_list_shaders_total, bb_list_pipelines_done,
    bb_list_pipelines_total;
extern std::unique_ptr<Vulkan::Presenter> presenter;
namespace Vulkan {

namespace {
// bbport BB_FIXED_QUAD_PROBE (diagnostics). Read once: getenv takes the process's environment lock,
// and the draw-preparation workers asked for it on every draw (4% of their time, partly waiting
// for each other on that lock).
bool FixedQuadProbe() {
    static const bool enabled = std::getenv("BB_FIXED_QUAD_PROBE") != nullptr;
    return enabled;
}
bool FixedQuadTrianglesEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_FIXED_QUAD_TRIANGLES");
        return env && env[0] == '1';
    }();
    return enabled;
}
// bbport: draws with a geometry shader stage are skipped where the GPU has none (Metal/MoltenVK).
// Logged once and then every 10000: a warning per draw (hundreds a frame) went through the log
// pipe into the launcher and cost frames by itself.
void NoteSkippedGeometryDraw() {
    static std::atomic<u64> skipped{0};
    const u64 n = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 10000 == 0) {
        LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported: draw skipped ({} so far)", n);
    }
}
} // namespace

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;


static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.hw.ls.ls_stride = tess_constants.ls_stride;
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !device.GetCapabilities().depth_clip_control &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        // The key's type: a quad list drawn as triangles has an ordinary vertex stage.
        info.sw.vs.tess_emulated_primitive =
            sel.graphics_key.prim_type == AmdGpu::PrimitiveType::RectList ||
            sel.graphics_key.prim_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

/// bbport BB_ASYNC_PIPELINES: worker threads creating graphics pipelines (the driver's compile,
/// vkCreateGraphicsPipelines, with its own VkPipelineCache synchronization). Jobs run in order.
class PipelineCompiler {
public:
    explicit PipelineCompiler(u32 threads) {
        for (u32 i = 0; i < threads; ++i) {
            workers.emplace_back([this] { Run(); });
        }
    }
    ~PipelineCompiler() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }
    void Submit(std::function<void()> job) {
        {
            std::scoped_lock lk{mutex};
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }

private:
    void Run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lk{mutex};
                cv.wait(lk, [&] { return stop || !jobs.empty(); });
                if (jobs.empty()) {
                    return; // stopping, every job done
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> workers;
    bool stop = false;
};

PipelineCache::PipelineCache(VideoCore::Backend::Device& device_, AmdGpu::Liverpool* liverpool_,
                             u32 sparse_page_shift)
    : device{device_}, liverpool{liverpool_} {
    sel.regs = &liverpool->regs;
    // What the device's shaders can do; the rest comes from the game and the memory model.
    profile = device.ShaderProfile();
    profile.sparse_page_shift = sparse_page_shift;
    // bbport BB_LAYER_MEMORY: buffers over nearly all memory go through the page table.
    profile.paged_buffers = VideoCore::BufferCache::LayerPagedActive();
    // The game's buffer copy shader, from its profile (games/).
    profile.buffer_copy_shader_hash = Game::BufferCopyShader();
    WarmUp();
}


struct PipelineCache::PendingPipeline {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::unique_ptr<GraphicsPipeline> pipeline;
    GraphicsPipeline::SerializationSupport sdata{};
    std::array<VideoCore::Backend::ShaderHandle, MaxShaderStages> modules{};
};

PipelineCache::~PipelineCache() = default;

// bbport: shader/pipeline compile time on the GPU thread, reported by BB_FRAME_STATS.
std::atomic<u64> g_bb_compile_ns;
std::atomic<u32> g_bb_compiles;
namespace {
struct CompileTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        g_bb_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
        ++g_bb_compiles;
    }
};
} // namespace

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // N2.3: a stage the worker filled from the draw state is filled again from this draw's
        // record and compared, without the walker; if it cannot be, the draw is prepared here.
        if (stage.from_state) {
            if (!BbRenderBoundary::GpuFlat(stage.program, info, stage.hw_stage)) {
                return nullptr;
            }
        } else {
            info.RefreshFlatBuf();
        }
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size ||
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    const auto it = graphics_pipelines.find(prepared.key);
    // A pipeline the driver could not create is empty: no pipeline, the draw is skipped.
    return it != graphics_pipelines.end() && it->second && it->second->Handle() ? it->second.get()
                                                                                : nullptr;
}

namespace {
/// bbport BB_ASYNC_PIPELINES: per render target (its address), how many frames in a row draws
/// went to it. Only the GPU thread (pipeline selection) touches it.
struct TargetStreak {
    u32 last_frame = ~0u;
    u32 frames = 0;
};
std::unordered_map<u64, TargetStreak> target_streaks;

/// The draw's render target: its first color buffer written, else its depth buffer, else 0.
u64 DrawTarget(const AmdGpu::Regs& regs) {
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
                return regs.color_buffers[cb].Address();
            }
        }
    }
    return regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthAddress() : 0;
}

/// Counts the frames in a row a target is drawn to (once per frame and target).
void NoteDrawTarget(const AmdGpu::Regs& regs) {
    static u64 last_target = 0;
    static u32 last_frame = ~0u;
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    const u64 target = DrawTarget(regs);
    if (target == 0 || (target == last_target && frame == last_frame)) {
        return;
    }
    last_target = target;
    last_frame = frame;
    auto& streak = target_streaks[target];
    if (streak.last_frame != frame) {
        streak.frames = streak.last_frame + 1 == frame ? streak.frames + 1 : 1;
        streak.last_frame = frame;
    }
}
} // namespace

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared,
                                                           bool indirect, bool allow_fixed_quad) {
    used_prepared = nullptr;
    sel.indirect = indirect;
    sel.allow_fixed_quad = allow_fixed_quad;
    NoteDrawTarget(liverpool->regs);
    // Prepared workers currently decline hull shaders. Keep this guard if they gain that support:
    // their original tessellation pipeline must not hide an opted-in fixed-quad selection.
    const bool try_prepared = !allow_fixed_quad || !FixedQuadTrianglesEnabled() || indirect ||
        liverpool->regs.stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::LsHs;
    if (prepared && try_prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    sel.draw_indirect_params = params;
    if (!RefreshGraphicsKey(sel)) {
        return nullptr;
    }
    if (const auto found = graphics_pipelines.find(sel.graphics_key); found != graphics_pipelines.end()) {
        // One the driver could not create stays empty: its draws are skipped every time, not
        // only the first (binding it crashed MoltenVK).
        return found->second && found->second->Handle() ? found->second.get() : nullptr;
    }
    // bbport BB_ASYNC_PIPELINES: being compiled on a worker, or to be.
    const bool skippable = AsyncSkippable(sel, indirect);
    if (const auto pending = pending_graphics.find(sel.graphics_key); pending != pending_graphics.end()) {
        const std::shared_ptr<PendingPipeline> job = pending->second;
        {
            std::unique_lock lk{job->mutex};
            if (!job->done) {
                if (skippable) {
                    ++async_skipped;
                    return nullptr; // drawn from the frame its pipeline is ready
                }
                // A pass that is not drawn every frame needs it now: the wait is compile time.
                const auto start = std::chrono::steady_clock::now();
                job->cv.wait(lk, [&] { return job->done; });
                g_bb_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count());
                ++async_waited;
            }
        }
        pending_graphics.erase(sel.graphics_key);
        return FinishPipeline(sel.graphics_key, *job);
    }
    if (skippable) {
        if (!compiler) {
            const char* env = std::getenv("BB_ASYNC_PIPELINE_THREADS");
            const u32 threads = env && *env ? u32(std::strtoul(env, nullptr, 10))
                                            : std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u);
            compiler = std::make_unique<PipelineCompiler>(std::max(threads, 1u));
            std::printf("GPU: new pipelines of passes drawn every frame compile on %u threads "
                        "(BB_ASYNC_PIPELINES=0: on the GPU thread, the game waits)\n",
                        std::max(threads, 1u));
        }
        auto job = std::make_shared<PendingPipeline>();
        job->modules = sel.modules;
        pending_graphics.emplace(sel.graphics_key, job);
        ++async_started;
        ++g_bb_compiles;
        compiler->Submit([this, job, key = sel.graphics_key, infos = sel.infos,
                          runtime_infos = sel.runtime_infos, fetch = sel.fetch_shader,
                          modules = sel.modules]() mutable {
            GraphicsPipeline::SerializationSupport sdata{};
            auto pipeline = std::make_unique<GraphicsPipeline>(
                device, profile, key, infos, runtime_infos, fetch,
                modules, sdata, false);
            std::scoped_lock lk{job->mutex};
            job->pipeline = std::move(pipeline);
            job->sdata = std::move(sdata);
            job->done = true;
            job->cv.notify_all();
        });
        sel.fetch_shader.reset();
        return nullptr;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        if (std::getenv("BB_PIPE_RECORD") && sel.infos[3] && !sel.infos[1] && !sel.infos[2]) {
            // Local experiment: a pipeline's whole state, for a replay before play.
            static FILE* record = std::fopen("out/pipeline-record.bin", "wb");
            const u64 vs = sel.infos[3]->pgm_hash;
            const u64 fs = sel.infos[0] ? sel.infos[0]->pgm_hash : 0;
            const u32 count = sel.fetch_shader ? u32(sel.fetch_shader->attributes.size()) : 0;
            std::fwrite(&vs, 8, 1, record);
            std::fwrite(&fs, 8, 1, record);
            std::fwrite(&sel.graphics_key, sizeof(sel.graphics_key), 1, record);
            std::fwrite(&count, 4, 1, record);
            for (u32 i = 0; i < count; ++i) {
                const u32 location = sel.fetch_shader->attributes[i].semantic;
                std::fwrite(&location, 4, 1, record);
            }
            std::fflush(record);
        }
        CompileTimer timer;

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            device, profile, sel.graphics_key, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false);

        // MoltenVK can reject a pipeline even when its shaders compiled. Never persist its key:
        // preloading that key would reject it again and discard the entire shader cache.
        if (!sel.graphics_key.fixed_quad_outer && it.value() && it.value()->Handle()) {
            RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        }
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    // A pipeline the driver could not create is empty: no pipeline, the draw is skipped.
    return it->second && it->second->Handle() ? it->second.get() : nullptr;
}

const GraphicsPipeline* PipelineCache::FinishPipeline(const GraphicsPipelineKey& key,
                                                      PendingPipeline& job) {
    const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(key);
    if (!key.fixed_quad_outer && job.pipeline && job.pipeline->Handle()) {
        RegisterPipelineData(key, pipeline_hash, job.sdata);
    }
    ++num_new_pipelines;
    if (EmulatorSettings.IsShaderCollect()) {
        for (const auto module : job.modules) {
            if (module) {
                module_related_pipelines[module].emplace_back(key);
            }
        }
    }
    auto& slot = graphics_pipelines[key];
    slot = std::move(job.pipeline);
    return slot && slot->Handle() ? slot.get() : nullptr;
}

bool PipelineCache::AsyncSkippable(const PipelineSelection& selection, bool indirect) {
    // Only a pass drawn every frame may go without a draw for a frame or two (its render target
    // drawn to in each of the last frames, below). A one-time render compiles at once, as during
    // loading screens (a frame of fewer than 300 draws before this one). The pipeline must not
    // depend on the draw's vertex buffers (VK_EXT_vertex_input_dynamic_state). A size rule (the
    // scene's, the output's) missed most passes: many set a 16384 scissor.
    static const bool enabled = [] {
        const char* env = std::getenv("BB_ASYNC_PIPELINES");
        return !(env && env[0] == '0');
    }();
    if (!enabled || !device.GetCapabilities().dynamic_vertex_input || !selection.regs) {
        return false;
    }
    static u32 frame_seen = ~0u;
    static u64 draws_at_start = 0, last_frame_draws = 0;
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    const u64 draws = BbStats::draws.load(std::memory_order_relaxed);
    if (frame != frame_seen) {
        last_frame_draws = draws - draws_at_start;
        draws_at_start = draws;
        frame_seen = frame;
        static u32 printed = 0;
        const u32 second = BbStats::coarse_second.load(std::memory_order_relaxed);
        if (async_started != 0 && second - printed >= 5) {
            printed = second;
            std::printf("GPU: %llu pipelines compiled in the background, %llu draws went without "
                        "theirs meanwhile, %llu waited for one\n",
                        (unsigned long long)async_started, (unsigned long long)async_skipped,
                        (unsigned long long)async_waited);
            async_started = async_skipped = async_waited = 0;
        }
    }
    if (last_frame_draws < 300) {
        return false;
    }
    // A full-screen pass (one triangle or quad: lighting, fog, post-processing) waits for its
    // pipeline: going without one for a frame left whole frames grey. So does an indirect draw
    // (its size unknown here). What may go without is geometry, which shows a frame later.
    constexpr u32 FullScreenIndices = 12;
    if (indirect || selection.regs->num_indices <= FullScreenIndices) {
        return false;
    }
    // A pass drawn every frame: its render target drawn to in each of the last 8 frames (the
    // scene, the output, shadow maps, half-size and depth-only passes alike). A one-time render
    // (a face baked for a character, a texture made once) goes to a target new or used now and
    // then: compiled at once, as before.
    constexpr u32 PerFrameStreak = 8;
    const u64 target = DrawTarget(*selection.regs);
    const auto streak = target_streaks.find(target);
    return target != 0 && streak != target_streaks.end() && streak->second.frames >= PerFrameStreak;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(
            device, profile, compute_key, *sel.infos[0],
            sel.modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool QuadsAsTriangles() {
    static const bool enabled = [] {
        // Off by default: the game draws no quad lists where measured (its tessellated draws
        // are rect lists, ~44 per frame in the clinic), so this path has not run there yet.
        const char* env = std::getenv("BB_QUADS_AS_TRIANGLES");
        return env && env[0] == '1';
    }();
    return enabled;
}

bool RectsAsTriangles() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_RECTS_AS_TRIANGLES");
        return !env || env[0] != '0';
    }();
    return enabled;
}

/// bbport: the vertex stage of a rect list can be drawn as triangles with corners the CPU adds:
/// a passthrough (Info::rect_position_attr) of 32-bit float attributes, position per vertex.
static bool RectPassthrough(const PipelineSelection& sel) {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (sel.regs->stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::Vs) {
        return false;
    }
    const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
    if (!vs || vs->rect_position_attr < 0 || !sel.fetch_shader) {
        return false;
    }
    bool position = false;
    for (const auto& attrib : sel.fetch_shader->attributes) {
        const bool is_position = attrib.semantic == u32(vs->rect_position_attr);
        if (attrib.GetStepRate() != InstanceIdType::None) {
            if (is_position) {
                return false;
            }
            continue;
        }
        const auto buffer = attrib.GetSharp(*vs);
        const u32 components = RectAttributeComponents(
            LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()));
        if (components == 0) {
            return false;
        }
        if (is_position) {
            position = vs->rect_position_comp[0] < components &&
                       vs->rect_position_comp[1] < components;
        }
    }
    return position;
}

u32 RectAttributeComponents(VideoCore::Backend::Format format) {
    using VideoCore::Backend::Format;
    switch (format) {
    case Format::R32Sfloat:
        return 1;
    case Format::R32G32Sfloat:
        return 2;
    case Format::R32G32B32Sfloat:
        return 3;
    case Format::R32G32B32A32Sfloat:
        return 4;
    default:
        return 0;
    }
}

u32 FixedQuadCpuComponents(AmdGpu::DataFormat data, AmdGpu::NumberFormat number) {
    if (data == AmdGpu::DataFormat::Format8_8_8_8 && number == AmdGpu::NumberFormat::Unorm)
        return 4;
    if (number != AmdGpu::NumberFormat::Float) return 0;
    switch (data) {
    case AmdGpu::DataFormat::Format32: return 1;
    case AmdGpu::DataFormat::Format32_32: return 2;
    case AmdGpu::DataFormat::Format32_32_32: return 3;
    case AmdGpu::DataFormat::Format32_32_32_32: return 4;
    default: return 0;
    }
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    // bbport: a quad list becomes a triangle list the draw indexes (QuadsAsTriangles): not for
    // indirect draws or with primitive restart.
    if (key.prim_type == AmdGpu::PrimitiveType::QuadList && QuadsAsTriangles() && !sel.indirect &&
        !(regs.enable_primitive_restart & 1)) {
        key.prim_type = AmdGpu::PrimitiveType::TriangleList;
    }
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    // bbport (RectsAsTriangles): a rect list whose vertex shader passes float attributes through
    // is drawn as a triangle list, its 4th corners written by the rasterizer (ExpandRects).
    if (key.prim_type == AmdGpu::PrimitiveType::RectList && RectsAsTriangles() && !sel.indirect &&
        RectPassthrough(sel)) {
        key.prim_type = AmdGpu::PrimitiveType::TriangleList;
        if (!RefreshGraphicsStages(sel) || !RectPassthrough(sel)) {
            key.prim_type = AmdGpu::PrimitiveType::RectList;
            if (!RefreshGraphicsStages(sel)) {
                return false;
            }
        }
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // BB_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("BB_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("BB_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            Backend::ColorWriteMask{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = Backend::ColorWriteMask::All;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !device.GetCapabilities().mixed_any_samples ||
            all_color_samples_same && depth_mismatch && !device.GetCapabilities().mixed_depth_samples) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;
    sel.fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    Shader::Backend::Bindings domain_binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        return true;
    };

    sel.infos.fill(nullptr);
    sel.modules.fill({});

    if (!bind_stage(HwStage::Fragment, SwStage::Fragment)) {
        // No pixel shader: the clip-distance discard shader may still be needed, decided from
        // this draw's registers, not a previous draw's fragment stage (bbport 0.6, 8185b88).
        sel.runtime_infos[u32(SwStage::Fragment)] =
            BuildRuntimeInfo(sel, HwStage::Fragment, SwStage::Fragment);
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!device.GetCapabilities().geometry_shaders) {
            NoteSkippedGeometryDraw();
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!device.GetCapabilities().tessellation) {
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        domain_binding = binding;
        if (!bind_stage(HwStage::Vertex, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!device.GetCapabilities().tessellation) {
            return false;
        }
        if (!device.GetCapabilities().geometry_shaders) {
            NoteSkippedGeometryDraw();
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(HwStage::Hull, SwStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(HwStage::Export, SwStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(HwStage::Local, SwStage::Vertex)) {
            return false;
        }
        if (!bind_stage(HwStage::Geometry, SwStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(HwStage::Vertex, SwStage::Vertex);
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    if (FixedQuadProbe() &&
        regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::LsHs) {
        static std::atomic<u32> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
            const auto* hs = sel.infos[u32(SwStage::TessellationControl)];
            const auto* ls = sel.infos[u32(SwStage::Vertex)];
            const auto* ds = sel.infos[u32(SwStage::TessellationEval)];
            std::printf("Fixed quad selection: primitive %u, restart %u, hull outer %u, "
                        "local outputs %u, domain stride %u, patches %u, fetch %u\n",
                        u32(key.prim_type), regs.enable_primitive_restart & 1,
                        hs ? hs->fixed_quad_outer : 0, ls ? ls->fixed_quad_output_count : 0,
                        sel.runtime_infos[u32(SwStage::TessellationEval)].sw.tes.hs_output_cp_stride,
                        ds ? ds->uses_patches : 0, bool(sel.fetch_shader));
        }
    }

    if (FixedQuadTrianglesEnabled() && sel.allow_fixed_quad && !sel.worker && !sel.indirect &&
        regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::LsHs &&
        key.prim_type == AmdGpu::PrimitiveType::PatchPrimitive &&
        !(regs.enable_primitive_restart & 1)) {
        const auto hs_index = u32(SwStage::TessellationControl);
        const auto ds_index = u32(SwStage::TessellationEval);
        const auto ls_index = u32(SwStage::Vertex);
        const auto* hs = sel.infos[hs_index];
        const auto* ds = sel.infos[ds_index];
        const auto* ls = sel.infos[ls_index];
        Shader::TessellationDataConstantBuffer tess_constants{};
        if (hs) hs->ReadTessConstantBuffer(tess_constants);
        const u32 count = Common::AlignUp(tess_constants.hs_cp_stride, 16) >> 4;
        bool eligible = hs && ds && ls && hs->fixed_quad_outer && !ds->uses_patches &&
                        !ds->loads.GetAny(Shader::IR::Attribute::InstanceId) &&
                        ls->fixed_quad_output_count == count && count < MaxVertexBufferCount &&
                        sel.fetch_shader;
        const char* reject = eligible ? "none" : "base";
        if (eligible) {
            using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
            std::array<bool, Shader::IR::NumParams> needed{};
            for (u32 i = 0; i < count * 4; ++i) {
                const auto source = ls->fixed_quad_outputs[i].source_attr;
                if (source != 0xFF) needed[source] = true;
            }
            for (const auto& attr : sel.fetch_shader->attributes) {
                if (attr.semantic >= needed.size() || !needed[attr.semantic]) continue;
                const auto buffer = attr.GetSharp(*ls);
                if (attr.GetStepRate() != InstanceIdType::None ||
                    FixedQuadCpuComponents(buffer.GetDataFmt(), buffer.GetNumberFmt()) == 0) {
                    eligible = false;
                    reject = "fetch format or step rate";
                    if (FixedQuadProbe()) {
                        static std::atomic<u32> printed_format{0};
                        if (printed_format.fetch_add(1, std::memory_order_relaxed) < 4)
                            std::printf("Fixed quad fetch rejection: semantic %u, format %u/%u, "
                                        "step %u\n", attr.semantic, u32(buffer.GetDataFmt()),
                                        u32(buffer.GetNumberFmt()), u32(attr.GetStepRate()));
                    }
                    break;
                }
            }
            for (u32 i = 0; eligible && i < count * 4; ++i) {
                const auto& source = ls->fixed_quad_outputs[i];
                if (source.source_attr == 0xFF) continue;
                const auto it = std::ranges::find_if(sel.fetch_shader->attributes,
                    [&](const auto& attr) { return attr.semantic == source.source_attr; });
                eligible = it != sel.fetch_shader->attributes.end() &&
                           source.source_comp < FixedQuadCpuComponents(
                               it->GetSharp(*ls).GetDataFmt(),
                               it->GetSharp(*ls).GetNumberFmt());
                if (!eligible) reject = "source component";
            }
        }
        if (FixedQuadProbe()) {
            static std::atomic<u32> reported_eligibility{0};
            if (reported_eligibility.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::printf("Fixed quad eligibility: %u, domain attributes %u, reject %s\n",
                            eligible, count, reject);
            }
        }
        if (eligible) {
            const u32 outer = hs->fixed_quad_outer;
            sel.runtime_infos[ds_index].InitFromTessConstants(tess_constants);
            const size_t module_key = HashCombine(static_cast<u64>(key.stage_hashes[ds_index]),
                                                  static_cast<u64>(outer));
            auto module = fixed_quad_modules.find(module_key);
            if (module == fixed_quad_modules.end()) {
                const auto params = AmdGpu::GetParams(regs.vs_program);
                Shader::Info vertex_info{HwStage::Vertex, SwStage::TessellationEval, params};
                auto vertex_runtime = sel.runtime_infos[ds_index];
                vertex_runtime.fixed_quad_vertex = true;
                vertex_runtime.fixed_quad_vertex_count = 0;
                auto vertex_binding = domain_binding;
                if (profile.stage_bindings) {
                    vertex_binding.EnterStage(u32(SwStage::TessellationEval));
                }
                const auto ir = Shader::TranslateProgram(params.code, pools, vertex_info,
                                                         vertex_runtime, profile);
                auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, vertex_runtime, ir,
                                                              vertex_binding);
                module = fixed_quad_modules
                             .emplace(module_key, MakeShader(spv, "fixed quad vertex shader"))
                             .first;
            }
            key.fixed_quad_outer = outer;
            key.prim_type = AmdGpu::PrimitiveType::TriangleList;
            sel.runtime_infos[ds_index].fixed_quad_vertex = true;
            sel.runtime_infos[ds_index].fixed_quad_vertex_count = 0;
            sel.modules[ds_index] = module->second;
            sel.modules[hs_index] = {};
            sel.modules[ls_index] = {};
        }
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    // bbport: a draw whose vertex buffer table pointer is null has no vertex input to build:
    // skipped (Supermedo/bloodborne_pc 2c4ddeb).
    if (vs_info && sel.fetch_shader) {
        for (const auto& attrib : sel.fetch_shader->attributes) {
            if (vs_info->UdPointerNull(attrib.sgpr_base)) {
                static std::atomic<u32> reported{0};
                if (reported.fetch_add(1, std::memory_order_relaxed) < 5) {
                    std::printf("GPU: draw skipped: its vertex buffer table pointer is null\n");
                }
                return false;
            }
        }
    }
    if (vs_info && sel.fetch_shader && !device.GetCapabilities().dynamic_vertex_input) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return true;
}

// bbport: BB_SHADER_LOG=1 (N2.5): one line per new shader variant, with its conversion time; for a
// program's first variant a summary of what it is specialized for, for a later one which parts
// differ from the first (what an ahead-of-time conversion would have to predict).
static bool ShaderLogOn() {
    static const bool on = [] {
        const char* env = std::getenv("BB_SHADER_LOG");
        return env && env[0] == '1';
    }();
    return on;
}

static void LogVariant(u64 pgm_hash, Shader::HwStage stage, size_t perm_idx, double ms,
                       const Shader::StageSpecialization& spec, const Shader::StageSpecialization* first) {
    std::string what;
    if (!first) {
        u32 attrib_int = 0, image_other = 0, image_int = 0, image_storage = 0, image_srgb = 0, formatted = 0;
        for (const auto& a : spec.vs_attribs) {
            attrib_int += a.num_class != AmdGpu::NumberClass::Float;
        }
        for (const auto& i : spec.images) {
            image_other += i.type != AmdGpu::ImageType::Color2D;
            image_int += i.is_integer;
            image_storage += i.is_storage;
            image_srgb += i.is_srgb;
        }
        for (const auto& b : spec.buffers) {
            formatted += b.is_formatted;
        }
        what = fmt::format("attribs {} ({} integer), images {} ({} not 2D, {} integer, {} storage, {} sRGB), buffers "
                           "{} ({} formatted), samplers {}, fmasks {}",
                           spec.vs_attribs.size(), attrib_int, spec.images.size(), image_other, image_int,
                           image_storage, image_srgb, spec.buffers.size(), formatted, spec.samplers.size(),
                           spec.fmasks.size());
    } else {
        what = "differs in";
        auto part = [&](bool differs, const char* name) {
            if (differs) {
                what += ' ';
                what += name;
            }
        };
        part(spec.vs_attribs != first->vs_attribs, "attribs");
        part(spec.runtime_info != first->runtime_info, "runtime-info");
        part(spec.fetch_shader_data != first->fetch_shader_data, "fetch");
        part(spec.fmasks != first->fmasks, "fmasks");
        part(spec.start != first->start, "bindings");
        part(spec.buffers != first->buffers, "buffers");
        part(spec.images != first->images, "images");
        part(spec.samplers != first->samplers, "samplers");
    }
    {
        // Local experiment: the whole key of a variant, for comparison with a list from the files.
        std::string d = fmt::format("{{\"hash\":\"{:016x}\",\"stage\":{},\"perm\":{},\"start\":[{},{},{}]",
                                    pgm_hash, static_cast<unsigned>(stage), perm_idx, spec.start.unified,
                                    spec.start.buffer, spec.start.user_data);
        const auto& rt = spec.runtime_info;
        d += ",\"attribs\":[";
        for (const auto& a : spec.vs_attribs) {
            d += fmt::format("[{},{}],", static_cast<u32>(a.num_class), a.divisor);
        }
        d += "],\"fetch\":[";
        if (spec.fetch_shader_data) {
            for (const auto& a : spec.fetch_shader_data->attributes) {
                d += fmt::format("[{},{},{},{}],", a.semantic, a.dest_vgpr, a.num_elements, a.instance_data);
            }
        }
        d += "],\"buffers\":[";
        for (const auto& s : spec.buffers) {
            d += fmt::format("[{},{},{},{}],", u32(s.stride), u32(s.is_formatted), u32(s.data_format), u32(s.num_format));
        }
        d += "],\"images\":[";
        for (const auto& s : spec.images) {
            d += fmt::format("[{},{},{},{},{},{}],", static_cast<u32>(s.type), int(s.is_integer), int(s.is_storage),
                             int(s.is_cube), int(s.is_srgb), s.num_bindings);
        }
        d += "],\"samplers\":[";
        for (const auto& s : spec.samplers) {
            d += fmt::format("[{},{}],", u32(s.force_unnormalized), u32(s.force_degamma));
        }
        d += "]";
        if (stage == Shader::HwStage::Fragment) {
            d += fmt::format(",\"fs\":{{\"n\":{},\"samples\":{},\"mrtz\":{},\"dual\":{},\"motion\":{},\"colours\":[",
                             rt.hw.fs.num_inputs, rt.hw.fs.num_samples, rt.hw.fs.mrtz_mask,
                             int(rt.hw.fs.dual_source_blending), int(rt.hw.fs.motion_vectors));
            for (const auto& c : rt.hw.fs.color_buffers) {
                d += fmt::format("[{},{},{},{},{},{},{},{}],", static_cast<u32>(c.data_format),
                                 static_cast<u32>(c.num_format), static_cast<u32>(c.export_format),
                                 static_cast<u32>(c.swizzle.r), static_cast<u32>(c.swizzle.g),
                                 static_cast<u32>(c.swizzle.b), static_cast<u32>(c.swizzle.a),
                                 static_cast<u32>(c.num_conversion));
            }
            d += "],\"inputs\":[";
            for (u32 i = 0; i < rt.hw.fs.num_inputs; ++i) {
                const auto& in = rt.hw.fs.inputs[i];
                d += fmt::format("[{},{},{}],", in.param_index, int(in.is_default), int(in.is_flat));
            }
            d += "]}";
        }
        if (stage == Shader::HwStage::Vertex) {
            d += fmt::format(",\"vs\":{{\"n\":{},\"motion\":{},\"clip\":{},\"tess\":{}}}", rt.hw.vs.num_outputs,
                             int(rt.hw.vs.motion_vectors), rt.hw.vs.user_clip_plane_mask,
                             int(rt.sw.vs.tess_emulated_primitive));
        }
        d += "}";
        std::printf("Variant key: %s\n", d.c_str());
    }
    std::printf("Shader variant: %016llx stage %u variant %zu, %.1f ms; %s\n",
                static_cast<unsigned long long>(pgm_hash), static_cast<unsigned>(stage), perm_idx, ms,
                what.c_str());
}

// Local experiment (BB_SHADER_LIST=1): the list's programs translated from the game's files before
// play; every real variant is then looked up by its SPIR-V.
static std::unordered_set<u64> list_spirv;
static std::unordered_map<u64, std::vector<u32>> list_candidates;
static u32 list_candidate = 0;
static std::array<u32, 4> list_format{};
static u32 list_image = 0;
static std::unordered_map<u64, std::vector<VideoCore::Backend::ShaderHandle>> list_modules;

void PipelineCache::ListHit(const Shader::Info& info, const std::vector<u32>& spv,
                            const Shader::RuntimeInfo& rt) {
    static u32 total[8]{};
    static u32 hits[8]{};
    if (!std::getenv("BB_SHADER_LIST")) {
        return;
    }
    const u32 stage = static_cast<u32>(info.hw_stage) & 7;
    if (info.pgm_hash == 0xae894a3aULL) {
        FILE* out = std::fopen("out/spv-real.spv", "wb");
        std::fwrite(spv.data(), 4, spv.size(), out);
        std::fclose(out);
    }
    ++total[stage];
    const bool hit = list_spirv.contains(XXH3_64bits(spv.data(), spv.size() * 4));
    hits[stage] += hit;
    if (!hit) {
        std::printf("List miss: %016llx stage %u\n", static_cast<unsigned long long>(info.pgm_hash), stage);
    }
    if (info.hw_stage == HwStage::Compute) {
        std::string line = fmt::format("{} {:x} ", int(hit), info.pgm_hash);
        for (const auto& d : info.buffers) {
            const auto s = d.GetSharp(info);
            line += fmt::format("B{}:{}:{}:{}{}{}{}:{},", u32(s.stride), u32(s.data_format), u32(s.num_format), u32(s.dst_sel_x),
                                u32(s.dst_sel_y), u32(s.dst_sel_z), u32(s.dst_sel_w), int(d.is_formatted));
        }
        for (const auto& d : info.images) {
            const auto s = d.GetSharp(info);
            line += fmt::format("I{}:{}:{}:{}{}{}{}:{},", u32(s.type), u32(s.data_format), u32(s.num_format), u32(s.dst_sel_x),
                                u32(s.dst_sel_y), u32(s.dst_sel_z), u32(s.dst_sel_w), int(d.is_written));
        }
        std::printf("Cs real: %s\n", line.c_str());
        const auto& cs = rt.hw.cs;
        std::printf("Cs key: %llx %u %u %u %u %u %u %u %u %u %u %u %u %u\n", static_cast<unsigned long long>(info.pgm_hash),
                    cs.workgroup_size[0], cs.workgroup_size[1], cs.workgroup_size[2], u32(cs.tgid_enable[0]),
                    u32(cs.tgid_enable[1]), u32(cs.tgid_enable[2]), cs.shared_memory_size, rt.props.num_user_data,
                    rt.props.num_allocated_vgprs, u32(rt.props.fp_denorm_mode32), u32(rt.props.fp_denorm_mode16_64),
                    u32(rt.props.fp_round_mode32), u32(rt.props.fp_round_mode16_64));
    }
    if (info.hw_stage == HwStage::Fragment) {
        const auto& fs = rt.hw.fs;
        std::string line = fmt::format("{} {:x} {} {} {} {} {} {} {} {} {} ", int(hit), info.pgm_hash, u32(fs.z_export_format),
                                       fs.num_samples, fs.mrtz_mask, int(fs.front_face_all_bits), int(fs.dual_source_blending),
                                       int(fs.clip_distance_emulation), int(fs.motion_vectors), rt.props.num_input_vgprs,
                                       u32(rt.props.fp_denorm_mode32));
        for (const auto& c : fs.color_buffers) {
            line += fmt::format("{}:{}:{}:{}:{},", u32(c.data_format), u32(c.num_format), u32(c.num_conversion), u32(c.export_format), u32(c.blend_self_scale));
        }
        std::printf("Fs key: %s\n", line.c_str());
    }
    if (const auto real = Shader::Gcn::ParseFetchShader(info); real) {
        {
            const auto& vs = rt.hw.vs;
            std::string line = fmt::format("{} {:x} {} {} {} {} {} {} {} {} ", int(hit), info.pgm_hash,
                                           rt.props.num_user_data, rt.props.num_input_vgprs, vs.num_outputs,
                                           vs.user_clip_plane_mask, int(vs.clip_disable), int(vs.motion_vectors),
                                           rt.sw.vs.vertex_sgpr_offset, int(rt.sw.vs.tess_emulated_primitive));
            for (const auto& a : real->attributes) {
                const auto b = info.ReadUdReg<AmdGpu::Buffer>(a.sgpr_base, a.dword_offset);
                line += fmt::format("{}:{}:{}:{}:{}:{},", u32(b.data_format), u32(b.num_format), u32(b.dst_sel_x), u32(b.dst_sel_y),
                                    u32(b.dst_sel_z), u32(b.dst_sel_w));
            }
            std::printf("Vs key: %s\n", line.c_str());
        }
        std::string line = fmt::format("{} {} {} ", int(hit), int(real->vertex_offset_sgpr),
                                       int(real->instance_offset_sgpr));
        for (const auto& a : real->attributes) {
            line += fmt::format("{}:{}:{}:{}:{}:{}:{},", a.sgpr_base, a.dword_offset, a.instance_data, a.inst_offset,
                                a.data_format, a.num_format, a.num_elements);
        }
        std::printf("Fetch key: %016llx %s\n", static_cast<unsigned long long>(info.pgm_hash), line.c_str());
    }
    std::printf("Shader list hits:");
    for (u32 s = 0; s < 8; ++s) {
        if (total[s]) {
            std::printf(" %u:%u/%u", s, hits[s], total[s]);
        }
    }
    std::printf("\n");
}

__attribute__((noinline)) int PipelineCache::ListTranslate(HwStage hw, SwStage sw,
                                                           const Shader::ShaderParams& params,
                                                           Shader::RuntimeInfo& rt,
                                                           std::span<const u32> code,
                                                           const Shader::Gcn::FetchShaderData* fetch,
                                                           const AmdGpu::Buffer* attribs) {
    static std::vector<u32> flat(256);
    static sigjmp_buf recover;
    if (const int why = sigsetjmp(recover, 1); why != 0) {
        runtime_fault_recover = nullptr;
        bb_assert_recover = nullptr;
        return why == 2 ? 2 : 1;
    }
    runtime_fault_recover = &recover;
    bb_assert_recover = &recover;
    Shader::Info guess{hw, sw, params};
    guess.list_mode = true;
    guess.flat_override = &flat;
    guess.fetch_override = fetch;
    guess.attrib_override = attribs;
    guess.list_stride = list_format[0];
    guess.list_data_format = list_format[1];
    guess.list_num_format = list_format[2];
    guess.list_image_type = list_image;
    guess.list_swizzle = list_format[3];
    Shader::Backend::Bindings next{};
    if (sw != SwStage::Compute) {
        next.EnterStage(u32(sw));
    }
    try {
        const auto ir = Shader::TranslateProgram(code, ahead_pools, guess, rt, profile);
        const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, ir, next);
        if (params.hash == 0xae894a3aULL) {
            static int dump = 0;
            FILE* out = std::fopen(fmt::format("out/spv-list-{}.spv", dump++).c_str(), "wb");
            std::fwrite(spv.data(), 4, spv.size(), out);
            std::fclose(out);
        }
        if (list_spirv.insert(XXH3_64bits(spv.data(), spv.size() * 4)).second &&
            std::getenv("BB_SHADER_LIST_COMPILE")) {
            ++bb_list_shaders_total;
            list_modules[params.hash].push_back(MakeShader(spv, GetShaderName(hw, params.hash, 0)));
            if (hw == HwStage::Compute) {
                static u64 fake = 0x8000000;
                MetalShadow::Compute(ComputePipelineKey{size_t(fake)}, NativeModule(list_modules[params.hash].back()),
                                     vk::Pipeline{reinterpret_cast<VkPipeline>(fake += 16)});
            }
            list_candidates[params.hash].push_back(list_candidate);
        }
    } catch (const std::exception& e) {
        runtime_fault_recover = nullptr;
        bb_assert_recover = nullptr;
        static int shown = 0;
        if (shown++ < 5) {
            std::printf("List exception: %llx %s\n", static_cast<unsigned long long>(params.hash), e.what());
        }
        return 3;
    }
    runtime_fault_recover = nullptr;
    bb_assert_recover = nullptr;
    return 0;
}

// The build runs on a thread of its own; this thread presents blank frames, so the overlay's bar
// keeps moving.
void PipelineCache::ListBuild() {
    static bool started = false;
    if (started || !std::getenv("BB_SHADER_LIST")) {
        return;
    }
    started = true;
    static std::atomic<bool> finished{false};
    static PipelineCache* self;
    self = this;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);
    pthread_t thread;
    pthread_create(&thread, &attr, [](void*) -> void* {
        self->ListBuildWork();
        finished.store(true);
        return nullptr;
    }, nullptr);
    while (!finished.load()) {
        presenter->Present(presenter->PrepareBlankFrame(false), false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    pthread_join(thread, nullptr);
    bb_list_shaders_total.store(0);
}

void PipelineCache::ListBuildWork() {
    static std::vector<u32> blob;
    {
        std::ifstream file("out/shader-code.bin", std::ios::binary | std::ios::ate);
        blob.resize(size_t(file.tellg()) / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(blob.data()), blob.size() * 4);
    }
    static std::array<u32, Shader::ShaderParams::NumShaderUserData> zero_ud{};
    const auto start_time = std::chrono::steady_clock::now();
    u32 programs = 0, translations = 0, faults = 0, asserts = 0, exceptions = 0;
    u32 skipped_kind[2]{};
    const auto split = [](const std::string& text) {
        std::vector<std::pair<int, int>> out;
        if (text == "-") {
            return out;
        }
        std::istringstream items(text);
        std::string item;
        while (std::getline(items, item, ',')) {
            const auto colon = item.find(':');
            out.emplace_back(std::stoi(item.substr(0, colon)),
                             colon == std::string::npos ? 0 : std::stoi(item.substr(colon + 1)));
        }
        return out;
    };
    // The translator can spin on code it was not meant to see: a stuck program is interrupted and
    // skipped through the fault path.
    static std::atomic<u64> watch_ticket{0};
    static std::atomic<bool> watch_stop{false};
    static std::atomic<u32> timeouts{0};
    const pthread_t target = pthread_self();
    std::thread watchdog([target] {
        u64 seen = 0;
        int ticks = 0;
        while (!watch_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const u64 now = watch_ticket.load();
            ticks = now == seen ? ticks + 1 : 0;
            seen = now;
            if (ticks >= 20) {
                ticks = 0;
                timeouts.fetch_add(1);
                pthread_kill(target, SIGSEGV);
            }
        }
    });
    std::ifstream list_file("out/shader-list2.txt");
    std::string line;
    while (std::getline(list_file, line)) {
        std::istringstream in(line);
        u64 hash;
        u32 kind, rsrc1, rsrc2, z_format, col, en, addr, inputs, exe;
        size_t offset, length;
        std::string order, a, b, c, d;
        in >> std::hex >> hash >> std::dec >> kind >> std::hex >> rsrc1 >> rsrc2 >> z_format >> col >> en >> addr >>
            std::dec >> inputs >> exe >> offset >> length >> order >> a >> b >> c >> d;
        if (kind == 4) {
            // Compute: every register is in the header; the formats are tried.
            static const std::array<std::array<u32, 4>, 16> formats{{{4, 10, 0, 0x4567}, {4, 10, 0, 0x6547},
                {4, 10, 4, 0x4567}, {4, 10, 9, 0x6547}, {4, 4, 7, 0x4001}, {4, 4, 4, 0x4000}, {4, 5, 7, 0x4501},
                {4, 5, 0, 0x4501}, {4, 6, 7, 0x4561}, {8, 11, 7, 0x4501}, {8, 12, 7, 0x4567}, {16, 14, 7, 0x4567},
                {16, 14, 4, 0x4567}, {12, 13, 4, 0x4560}, {1, 1, 0, 0x4001}, {1, 1, 0, 0x0004}}};
            static const std::array<u32, 5> images{9, 13, 10, 8, 12};
            const std::span<const u32> code{blob.data() + offset / 4, length / 4};
            ++programs;
            for (const auto& format : formats) {
                for (const u32 image : images) {
                    ++translations;
                    Shader::RuntimeInfo rt{};
                    rt.Initialize(HwStage::Compute, SwStage::Compute);
                    rt.props.num_user_data = (rsrc2 >> 1) & 0x1f;
                    rt.props.num_allocated_vgprs = (rsrc1 & 0x3f) * 4;
                    rt.props.fp_round_mode32 = AmdGpu::FpRoundMode((rsrc1 >> 12) & 3);
                    rt.props.fp_round_mode16_64 = AmdGpu::FpRoundMode((rsrc1 >> 14) & 3);
                    rt.props.fp_denorm_mode32 = AmdGpu::FpDenormMode((rsrc1 >> 16) & 3);
                    rt.props.fp_denorm_mode16_64 = AmdGpu::FpDenormMode((rsrc1 >> 18) & 3);
                    rt.hw.cs.workgroup_size = {z_format, col, en};
                    rt.hw.cs.tgid_enable = {bool(rsrc2 >> 7 & 1), bool(rsrc2 >> 8 & 1), bool(rsrc2 >> 9 & 1)};
                    const Shader::ShaderParams params{
                        .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{
                            zero_ud.data(), Shader::ShaderParams::NumShaderUserData},
                        .code = code,
                        .hash = hash,
                    };
                    list_format = format;
                    list_image = image;
                    watch_ticket.fetch_add(1);
                    const int why = ListTranslate(HwStage::Compute, SwStage::Compute, params, rt, code, nullptr, nullptr);
                    list_format = {};
                    list_image = 0;
                    if (why != 0) {
                        ++(why == 2 ? asserts : why == 3 ? exceptions : faults);
                    }
                }
            }
            continue;
        }
        const bool fragment = kind == 2;
        const auto hw = fragment ? HwStage::Fragment : HwStage::Vertex;
        const auto sw = fragment ? SwStage::Fragment : SwStage::Vertex;
        const std::span<const u32> code{blob.data() + offset / 4, length / 4};
        const auto input_order = split(order);
        const std::array attribute_sets{split(a), split(b), split(c), split(d)};
        const u32 candidates = fragment ? (col != 0 ? 5 : 1) : 4;
        ++programs;
        for (u32 candidate = 0; candidate < candidates; ++candidate) {
            ++translations;
            const bool front_face = fragment && (z_format >> 8 & 1) != 0;
            const u32 colour_candidate = candidate;
            Shader::RuntimeInfo rt{};
            rt.Initialize(hw, sw);
            rt.props.num_user_data = (rsrc2 >> 1) & 0x1f;
            rt.props.num_allocated_vgprs = ((rsrc1 & 0x3f) + 1) * 4;
            rt.props.fp_round_mode32 = AmdGpu::FpRoundMode((rsrc1 >> 12) & 3);
            rt.props.fp_round_mode16_64 = AmdGpu::FpRoundMode((rsrc1 >> 14) & 3);
            rt.props.fp_denorm_mode32 = AmdGpu::FpDenormMode((rsrc1 >> 16) & 3);
            rt.props.fp_denorm_mode16_64 = AmdGpu::FpDenormMode((rsrc1 >> 18) & 3);
            std::optional<Shader::Gcn::FetchShaderData> fetch;
            std::vector<AmdGpu::Buffer> attribs(256);
            if (fragment) {
                std::memcpy(&rt.hw.fs.en_flags, &en, sizeof(en));
                std::memcpy(&rt.hw.fs.addr_flags, &addr, sizeof(addr));
                rt.hw.fs.num_inputs = inputs;
                rt.hw.fs.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(z_format & 0xff);
                for (size_t i = 0; i < input_order.size(); ++i) {
                    rt.hw.fs.inputs[i].param_index = u8(input_order[i].first);
                }
                rt.hw.fs.front_face_all_bits = front_face;
                for (u32 i = 0; colour_candidate != 1 && i < Shader::MaxColorBuffers; ++i) {
                    const u32 format = (col >> (4 * i)) & 0xf;
                    if (format != 0) {
                        auto& colour = rt.hw.fs.color_buffers[i];
                        colour.data_format = AmdGpu::DataFormat::Format8_8_8_8;
                        colour.num_format = AmdGpu::NumberFormat::Unorm;
                        colour.export_format = static_cast<AmdGpu::ShaderExportFormat>(format);
                        colour.swizzle = AmdGpu::IdentityMapping;
                        if (colour_candidate == 2) { // one 8-bit target in blue-first order
                            std::swap(colour.swizzle.r, colour.swizzle.b);
                        }
                        if (colour_candidate == 3) { // the G-buffer's targets
                            if (i == 2) {
                                colour.num_format = AmdGpu::NumberFormat::Srgb;
                            }
                            if (i == 4) {
                                colour.data_format = AmdGpu::DataFormat::Format10_11_11;
                                colour.num_format = AmdGpu::NumberFormat::Float;
                                colour.swizzle.a = AmdGpu::CompSwizzle::Zero;
                            }
                            if (i >= 5) {
                                colour.data_format = AmdGpu::DataFormat::Format16_16_16_16;
                                colour.num_format = AmdGpu::NumberFormat::Float;
                            }
                        }
                        if (colour_candidate == 4) { // 16-bit float targets
                            colour.data_format = AmdGpu::DataFormat::Format16_16_16_16;
                            colour.num_format = AmdGpu::NumberFormat::Float;
                        }
                    }
                }
            } else {
                rt.props.num_input_vgprs = exe ? 0 : 3;
                fetch.emplace();
                u32 index = 0;
                for (const auto [size, number] : attribute_sets[candidate]) {
                    Shader::Gcn::VertexAttribute attrib{};
                    attrib.semantic = u8(index);
                    attrib.dest_vgpr = u8(4 + 4 * index);
                    attrib.num_elements = 4;
                    attrib.dword_offset = u8(index * 4);
                    fetch->attributes.push_back(attrib);
                    auto& v = attribs[attrib.dword_offset];
                    v.base_address = 1;
                    v.num_records = 1;
                    v.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
                    v.num_format = u32(number == 2   ? AmdGpu::NumberFormat::Uint
                                       : number == 1 ? AmdGpu::NumberFormat::Sint
                                                     : AmdGpu::NumberFormat::Float);
                    using S = AmdGpu::CompSwizzle;
                    v.dst_sel_x = u32(S::Red);
                    v.dst_sel_y = u32(size >= 2 ? S::Green : S::Zero);
                    v.dst_sel_z = u32(size >= 3 ? S::Blue : S::Zero);
                    v.dst_sel_w = u32(size >= 4 ? S::Alpha : number != 0 ? S::Zero : S::One);
                    ++index;
                }
            }
            const Shader::ShaderParams params{
                .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{
                    zero_ud.data(), Shader::ShaderParams::NumShaderUserData},
                .code = code,
                .hash = hash,
            };
            {
                static FILE* progress = std::fopen("out/list-progress.txt", "w");
                std::fprintf(progress, "%llx %u %u\n", static_cast<unsigned long long>(hash), candidate, programs);
                std::fflush(progress);
            }
            watch_ticket.fetch_add(1);
            list_candidate = candidate;
            const int why = ListTranslate(hw, sw, params, rt, code, fetch ? &*fetch : nullptr, attribs.data());
            if (why != 0) {
                ++(why == 2 ? asserts : why == 3 ? exceptions : faults);
                skipped_kind[fragment] += candidate == 0;
            }
        }
    }
    watch_stop.store(true);
    watchdog.join();
    if (std::getenv("BB_PIPE_REPLAY")) {
        {
            std::ifstream count("out/pipeline-record.bin", std::ios::binary);
            u32 entries = 0;
            u64 skip[2];
            std::vector<char> key(sizeof(GraphicsPipelineKey));
            u32 attributes = 0;
            while (count.read(reinterpret_cast<char*>(skip), 16)) {
                count.read(key.data(), key.size());
                count.read(reinterpret_cast<char*>(&attributes), 4);
                count.seekg(attributes * 4, std::ios::cur);
                ++entries;
            }
            bb_list_pipelines_total.store(entries);
        }
        std::ifstream record("out/pipeline-record.bin", std::ios::binary);
        u32 built = 0, skipped = 0;
        u64 fake = 0x7000000;
        const auto replay_start = std::chrono::steady_clock::now();
        for (;;) {
            u64 vs = 0, fs = 0;
            u32 count = 0;
            auto key = std::make_unique<GraphicsPipelineKey>();
            if (!record.read(reinterpret_cast<char*>(&vs), 8)) {
                break;
            }
            record.read(reinterpret_cast<char*>(&fs), 8);
            record.read(reinterpret_cast<char*>(key.get()), sizeof(*key));
            ++bb_list_pipelines_done;
            record.read(reinterpret_cast<char*>(&count), 4);
            std::vector<MetalShadow::VertexInput> inputs(count);
            for (auto& in : inputs) {
                record.read(reinterpret_cast<char*>(&in.location), 4);
            }
            const auto vs_it = list_modules.find(vs);
            const auto fs_it = list_modules.find(fs);
            if (vs_it == list_modules.end() || (fs && fs_it == list_modules.end())) {
                ++skipped;
                continue;
            }
            size_t vertex_index = 0;
            for (const auto vertex : vs_it->second) {
                if (vertex_index++ > 0 && std::getenv("BB_PIPE_FIRST")) {
                    break;
                }
                const size_t fragments = fs ? fs_it->second.size() : 1;
                // A pixel shader's candidate follows from the state's colour targets.
                u32 wanted = 0;
                u32 targets = 0;
                for (const auto& c : key->color_buffers) {
                    targets += c.data_format != AmdGpu::DataFormat::FormatInvalid;
                }
                const auto& first = key->color_buffers[0];
                if (targets == 0) {
                    wanted = 1;
                } else if (targets >= 6) {
                    wanted = 3;
                } else if (first.swizzle.r == AmdGpu::CompSwizzle::Blue) {
                    wanted = 2;
                } else if (first.data_format == AmdGpu::DataFormat::Format16_16_16_16) {
                    wanted = 4;
                }
                for (size_t i = 0; i < fragments; ++i) {
                    if (fs && fragments > 1 && list_candidates[fs][i] != wanted &&
                        std::ranges::find(list_candidates[fs], wanted) != list_candidates[fs].end()) {
                        continue;
                    }
                    std::array<vk::ShaderModule, MaxShaderStages> modules{};
                    std::array<bool, MaxShaderStages> has{};
                    modules[3] = NativeModule(vertex);
                    has[3] = true;
                    if (fs) {
                        modules[0] = NativeModule(fs_it->second[i]);
                        has[0] = true;
                    }
                    MetalShadow::Graphics(*key, modules, has, inputs, false,
                                          vk::Pipeline{reinterpret_cast<VkPipeline>(fake += 16)});
                    ++built;
                }
            }
        }
        std::fflush(stdout);
        std::printf("Shader list pipelines: %u built, %u pipelines skipped, %.1f s\n", built, skipped,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - replay_start).count());
    }
    if (const char* wait = std::getenv("BB_SHADER_LIST_COMPILE")) {
        (void)wait;
        unsigned last = 0;
        int still = 0;
        while (bb_list_shaders_done.load() < bb_list_shaders_total.load() && still < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const unsigned now = bb_list_shaders_done.load();
            still = now == last ? still + 1 : 0;
            last = now;
        }
        if (std::getenv("BB_SHADER_LIST_EXIT")) {
            std::printf("Shader list: exiting before play\n");
            std::fflush(stdout);
            std::_Exit(0);
        }
    }
    std::printf("Shader list timeouts: %u, exceptions: %u\n", timeouts.load(), exceptions);
    std::printf("Shader list built: %u programs, %u translations, %zu modules, %u faults, %u asserts, "
                "first candidates skipped vertex %u pixel %u, %.1f s\n", programs, translations, list_spirv.size(),
                faults, asserts, skipped_kind[0], skipped_kind[1],
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count());
}

// Local experiment (BB_SHADER_LIST_CHECK=1): would a translation fed only a list's descriptors,
// with no user data and no game memory, produce this variant's SPIR-V?
template <typename Sharp, typename Fetch>
static void PutSharp(std::vector<u32>& flat, const Fetch& fetch, const Sharp& sharp) {
    std::array<u32, sizeof(Sharp) / 4> dw;
    std::memcpy(dw.data(), &sharp, sizeof(sharp));
    for (u32 i = 0; i < dw.size(); ++i) {
        if ((fetch.load_mask >> i & 1) && fetch.offsets[i] != Shader::UNKNOWN_LOCATION &&
            fetch.offsets[i] < flat.size()) {
            flat[fetch.offsets[i]] = dw[i];
        }
    }
}

static AmdGpu::Buffer CanonBuffer(AmdGpu::Buffer b, u32 level) {
    AmdGpu::Buffer c = b;
    if (level >= 1) {
        c.base_address = 1;
        c.num_records = 1;
    }
    if (level >= 2) {
        c.cache_swizzle = 0;
        c.add_tid_enable = 0;
        c.mtype = 0;
    }
    if (level == 6) {
        c.stride = 0;
    }
    if (level == 8 || level == 11) {
        c.stride = 16;
    }
    return c;
}

static AmdGpu::Image CanonImage(AmdGpu::Image i, u32 level) {
    AmdGpu::Image c = i;
    if (level >= 1) {
        c.base_address = 1;
        c.width = 0;
        c.height = 0;
        c.pitch = 0;
        c.min_lod = 0;
        c.min_lod_warn = 0;
    }
    if (level >= 2) {
        c.base_level = 0;
        c.last_level = 0;
        c.tiling_index = u64(AmdGpu::TileMode::Thin1DThin);
        c.mtype = 0;
        c.mtype2 = 0;
        c.mtype_l2 = 0;
        c.perf_modulation = 0;
        c.pow2pad = 0;
        c.atc = 0;
    }
    if (level == 3 && c.num_format == u64(AmdGpu::NumberFormat::Srgb)) {
        c.num_format = u64(AmdGpu::NumberFormat::Unorm);
    }
    if (level == 4) {
        c.dst_sel_x = u64(AmdGpu::CompSwizzle::Red);
        c.dst_sel_y = u64(AmdGpu::CompSwizzle::Green);
        c.dst_sel_z = u64(AmdGpu::CompSwizzle::Blue);
        c.dst_sel_w = u64(AmdGpu::CompSwizzle::Alpha);
    }
    if (level == 5 && c.num_format != u64(AmdGpu::NumberFormat::Uint) &&
        c.num_format != u64(AmdGpu::NumberFormat::Sint)) {
        c.data_format = u64(AmdGpu::DataFormat::Format8_8_8_8);
        c.num_format = u64(AmdGpu::NumberFormat::Unorm);
    }
    return c;
}

void PipelineCache::ListCheck(const Shader::Info& info, const Shader::RuntimeInfo& runtime_info,
                              std::span<const u32> code, const Shader::Backend::Bindings& start,
                              const std::vector<u32>& spv) {
    static const bool on = [] {
        const char* env = std::getenv("BB_SHADER_LIST_CHECK");
        return env && env[0] == '1';
    }();
    if (!on || info.sw_stage == SwStage::TessellationControl ||
        info.sw_stage == SwStage::TessellationEval) {
        return;
    }
    static u32 total[8]{};
    static u32 same[25][8]{};
    static u32 failed = 0;
    static std::array<u32, Shader::ShaderParams::NumShaderUserData> zero_ud{};
    const u32 stage = static_cast<u32>(info.hw_stage) & 7;
    const auto real_flat = info.FlatUserData();
    const auto fetch = Shader::Gcn::ParseFetchShader(info);
    static u32 list_missing = 0;
    u64 cube_seen = 0;
    u64 cube_real = 0;
    const bool plain_stage = info.hw_stage == HwStage::Vertex || info.hw_stage == HwStage::Fragment ||
                             info.hw_stage == HwStage::Compute;
    const auto attempt = [&](u32 level) {
        if (level >= 12 && !plain_stage) {
            return false; // tessellation and geometry stay at first use
        }
        std::vector<u32> flat(real_flat.size());
        if (level == 0) {
            std::ranges::copy(real_flat, flat.begin());
        }
        std::vector<AmdGpu::Buffer> attribs(256);
        for (const auto& desc : info.buffers) {
            AmdGpu::Buffer raw{};
            if (level && desc.sharp_fetch.Fetch(real_flat.data(), &raw)) {
                PutSharp(flat, desc.sharp_fetch,
                         CanonBuffer(raw, stage == 6 && level >= 8 ? 2 : level));
            }
        }
        for (const auto& desc : info.images) {
            AmdGpu::Image raw{};
            if (level && desc.sharp_fetch.Fetch(real_flat.data(), &raw)) {
                PutSharp(flat, desc.sharp_fetch, CanonImage(raw, level));
            }
        }
        for (const auto& desc : info.samplers) {
            AmdGpu::Sampler raw{};
            if (level && desc.sharp_fetch.Fetch(real_flat.data(), &raw)) {
                PutSharp(flat, desc.sharp_fetch, raw);
            }
        }
        if (fetch) {
            for (const auto& attrib : fetch->attributes) {
                attribs[attrib.dword_offset] =
                    CanonBuffer(info.ReadUdReg<AmdGpu::Buffer>(attrib.sgpr_base, attrib.dword_offset),
                                level == 6 ? 2 : level);
                if (level == 7) {
                    attribs[attrib.dword_offset].data_format =
                        u32(AmdGpu::DataFormat::Format32_32_32_32);
                    attribs[attrib.dword_offset].num_format = u32(AmdGpu::NumberFormat::Float);
                }
                if (level == 9 || level == 11 || level >= 12) {
                    auto& a = attribs[attrib.dword_offset];
                    const auto number = AmdGpu::GetNumberClass(a.GetNumberFmt());
                    a.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
                    a.num_format = u32(number == AmdGpu::NumberClass::Float
                                           ? AmdGpu::NumberFormat::Float
                                           : number == AmdGpu::NumberClass::Sint
                                                 ? AmdGpu::NumberFormat::Sint
                                                 : AmdGpu::NumberFormat::Uint);
                }
            }
        }
        const Shader::ShaderParams params{
            .user_data = std::span<const u32, Shader::ShaderParams::NumShaderUserData>{
                zero_ud.data(), Shader::ShaderParams::NumShaderUserData},
            .code = code,
            .hash = info.pgm_hash,
        };
        auto rt = runtime_info;
        struct ListEntry {
            u32 kind, user, vgprs, en, addr, inputs;
            std::vector<int> order;
            std::vector<std::pair<int, int>> attributes;
        };
        static const auto list = [] {
            std::unordered_map<u64, ListEntry> out;
            std::ifstream file("out/shader-list.txt");
            std::string line;
            while (std::getline(file, line)) {
                std::istringstream in(line);
                u64 hash;
                ListEntry e{};
                std::string order;
                std::string attributes;
                in >> std::hex >> hash >> std::dec >> e.kind >> e.user >> e.vgprs >> std::hex >> e.en >>
                    e.addr >> std::dec >> e.inputs >> order >> attributes;
                if (order == "-") {
                    order.clear();
                }
                if (attributes != "-") {
                    std::istringstream items(attributes);
                    std::string item;
                    while (std::getline(items, item, ',')) {
                        const auto colon = item.find(':');
                        e.attributes.emplace_back(std::stoi(item.substr(0, colon)),
                                                  std::stoi(item.substr(colon + 1)));
                    }
                }
                std::istringstream parts(order);
                std::string part;
                while (std::getline(parts, part, ',')) {
                    e.order.push_back(std::stoi(part));
                }
                out.emplace(hash, e);
            }
            return out;
        }();
        if (level >= 19 && info.hw_stage != HwStage::Compute) {
            const auto found = list.find(info.pgm_hash);
            if (found != list.end()) {
                const auto& e = found->second;
                rt.props.num_user_data = e.user;
                rt.props.num_allocated_vgprs = e.vgprs;
                if (info.hw_stage == HwStage::Fragment) {
                    std::memcpy(&rt.hw.fs.en_flags, &e.en, sizeof(e.en));
                    std::memcpy(&rt.hw.fs.addr_flags, &e.addr, sizeof(e.addr));
                    rt.hw.fs.num_inputs = e.inputs;
                    for (u32 i = 0; i < e.order.size() && i < rt.hw.fs.inputs.size(); ++i) {
                        rt.hw.fs.inputs[i].param_index = u8(e.order[i]);
                    }
                }
            } else {
                ++list_missing;
            }
            if (level == 20 || level == 21) {
                rt.props.num_input_vgprs = 0;
            }
            if (level == 21 && info.hw_stage == HwStage::Vertex) {
                rt.hw.vs.num_outputs = 0;
                rt.hw.vs.outputs = {};
            }
        }
        if (info.hw_stage == HwStage::Fragment && level >= 14) {
            if (level == 14 || level == 18) { // colour: only each target's number class
                for (auto& colour : rt.hw.fs.color_buffers) {
                    if (colour.data_format != AmdGpu::DataFormat::FormatInvalid) {
                        const auto number = AmdGpu::GetNumberClass(colour.num_format);
                        colour.data_format = AmdGpu::DataFormat::Format8_8_8_8;
                        colour.num_format = number == AmdGpu::NumberClass::Float
                                                ? AmdGpu::NumberFormat::Unorm
                                                : number == AmdGpu::NumberClass::Sint
                                                      ? AmdGpu::NumberFormat::Sint
                                                      : AmdGpu::NumberFormat::Uint;
                        colour.num_conversion = AmdGpu::NumberConversion::None;
                    }
                }
            }
            if (level == 15) { // colour: also a plain swizzle
                for (auto& colour : rt.hw.fs.color_buffers) {
                    colour.swizzle = AmdGpu::IdentityMapping;
                }
            }
            if (level == 16 || level == 18) { // inputs: only flat and default kept, plain order
                for (u32 i = 0; i < rt.hw.fs.num_inputs; ++i) {
                    rt.hw.fs.inputs[i].param_index = u8(i);
                }
            }
            if (level == 17) { // inputs dropped: does the order reach the SPIR-V at all?
                for (u32 i = 0; i < rt.hw.fs.num_inputs; ++i) {
                    rt.hw.fs.inputs[i].param_index = u8(i + 1);
                }
            }
        }
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            ++failed;
            return false;
        }
        runtime_fault_recover = &recover;
        if (level >= 22 && fetch) {
            const auto found = list.find(info.pgm_hash);
            size_t index = 0;
            for (const auto& attrib : fetch->attributes) {
                auto& a = attribs[attrib.dword_offset];
                if (found != list.end() && index < found->second.attributes.size()) {
                    const auto [size, number] = found->second.attributes[index];
                    a.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
                    if (level != 24) { // 23: class real... 24: size real
                    }
                    if (level == 22 || level == 24) {
                        a.num_format = u32(number == 2   ? AmdGpu::NumberFormat::Uint
                                           : number == 1 ? AmdGpu::NumberFormat::Sint
                                                         : AmdGpu::NumberFormat::Float);
                    }
                    using S = AmdGpu::CompSwizzle;
                    if (level == 22 || level == 23) {
                        a.dst_sel_x = u32(S::Red);
                        a.dst_sel_y = u32(size >= 2 ? S::Green : S::Zero);
                        a.dst_sel_z = u32(size >= 3 ? S::Blue : S::Zero);
                        a.dst_sel_w = u32(size >= 4 ? S::Alpha : S::One);
                    }
                }
                ++index;
            }
        }
        Shader::Info guess{info.hw_stage, info.sw_stage, params};
        guess.flat_override = &flat;
        auto fetch_copy = fetch;
        if (fetch_copy && (level == 10 || level == 11 || level >= 12)) {
            for (auto& attrib : fetch_copy->attributes) {
                attrib.num_elements = 4;
            }
        }
        guess.fetch_override = fetch_copy ? &*fetch_copy : nullptr;
        guess.attrib_override = attribs.data();
        u64 cubes = 0;
        for (size_t i = 0; i < info.images.size() && i < 64; ++i) {
            cubes |= u64(info.images[i].GetSharp(info).IsCube()) << i;
        }
        guess.list_mode = level >= 12 && !(level >= 14 && info.hw_stage == HwStage::Compute);
        guess.list_cubes = level == 12 ? &cubes : nullptr;
        auto next = start;
        const auto ir = Shader::TranslateProgram(code, ahead_pools, guess, rt, profile);
        const auto result = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, ir, next);
        runtime_fault_recover = nullptr;
        if (level == 13) {
            cube_seen = guess.list_found_cubes;
            cube_real = cubes;
        }
        return result == spv;
    };
    if (fetch) {
        std::string line;
        for (const auto& attrib : fetch->attributes) {
            const auto a = info.ReadUdReg<AmdGpu::Buffer>(attrib.sgpr_base, attrib.dword_offset);
            line += fmt::format("[{},{},{},{},{},{},{},{}],", attrib.semantic, u32(a.data_format),
                                u32(a.num_format), u32(a.dst_sel_x), u32(a.dst_sel_y), u32(a.dst_sel_z),
                                u32(a.dst_sel_w), u32(a.stride));
        }
        std::printf("Attr key: %016llx %s\n", static_cast<unsigned long long>(info.pgm_hash), line.c_str());
    }
    {
        const auto& p = runtime_info.props;
        std::printf("Regs key: %016llx %u %u %u %u %u %u %u %u", static_cast<unsigned long long>(info.pgm_hash),
                    stage, p.num_user_data, p.num_input_vgprs, p.num_allocated_vgprs, u32(p.fp_denorm_mode32),
                    u32(p.fp_denorm_mode16_64), u32(p.fp_round_mode32), u32(p.fp_round_mode16_64));
        if (info.hw_stage == HwStage::Fragment) {
            const auto& fs = runtime_info.hw.fs;
            u32 en = 0;
            u32 addr = 0;
            std::memcpy(&en, &fs.en_flags, sizeof(en));
            std::memcpy(&addr, &fs.addr_flags, sizeof(addr));
            std::printf(" %08x %08x %u %u %u", en, addr, fs.num_inputs, u32(fs.z_export_format), fs.mrtz_mask);
        }
        if (info.hw_stage == HwStage::Vertex) {
            const auto& vs = runtime_info.hw.vs;
            std::printf(" %u", vs.num_outputs);
            for (const auto& map : vs.outputs) {
                for (const auto out : map) {
                    std::printf(",%u", u32(out));
                }
            }
        }
        std::printf("\n");
    }
    ++total[stage];
    static u32 cube_sets_right = 0;
    u32 sums[25]{};
    u32 all = 0;
    for (u32 level = 0; level < 25; ++level) {
        same[level][stage] += attempt(level);
    }
    for (u32 s = 0; s < 8; ++s) {
        all += total[s];
        for (u32 level = 0; level < 25; ++level) {
            sums[level] += same[level][s];
        }
    }
    cube_sets_right += cube_seen == cube_real;
    std::printf("Cube positions right in %u variants\n", cube_sets_right);
    std::printf("Shader list check: %u variants, real %u, no addresses %u, key %u, no sRGB %u, plain swizzle %u, "
                "plain image format %u, no buffer stride %u, float attributes %u, stride 16 %u, attributes by class %u, "
                "four elements %u, all rules %u, list mode %u, list cubes from code %u, colour class %u, colour swizzle %u, "
                "plain inputs %u, inputs plus one %u, class and plain inputs %u, from the list %u, no input vgprs %u, "
                "no output map %u, attributes from the list %u, sizes only %u, classes only %u, list misses %u, faults %u; sizes by stage",
                all, sums[0], sums[1], sums[2], sums[3], sums[4], sums[5], sums[6], sums[7], sums[8], sums[9],
                sums[10], sums[11], sums[12], sums[13], sums[14], sums[15], sums[16], sums[17], sums[18], sums[19], sums[20], sums[21], sums[22], sums[23], sums[24], list_missing, failed);
    for (u32 s = 0; s < 8; ++s) {
        if (total[s]) {
            std::printf(" %u:%u/%u", s, same[23][s], total[s]);
        }
    }
    std::printf("\n");
}

VideoCore::Backend::ShaderHandle PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding, bool cached) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer;

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    const auto binding_before_emit = binding;
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    const bool fixed_quad_probe = FixedQuadProbe();
    if (fixed_quad_probe && info.sw_stage == SwStage::TessellationControl) {
        std::printf("Fixed quad hull eligibility: outer %u\n", info.fixed_quad_outer);
    }
    if (fixed_quad_probe && info.hw_stage == HwStage::Local) {
        std::printf("Fixed quad local output map: %u attributes\n",
                    info.fixed_quad_output_count);
    }
    if (fixed_quad_probe && info.sw_stage == SwStage::TessellationEval &&
        runtime_info.sw.tes.tess_type == AmdGpu::TessellationType::Quad &&
        info.uses_patches == 0) {
        auto vertex_info = runtime_info;
        vertex_info.fixed_quad_vertex = true;
        vertex_info.fixed_quad_vertex_count = 36;
        auto probe_binding = binding_before_emit;
        auto vertex_spv =
            Shader::Backend::SPIRV::EmitSPIRV(profile, vertex_info, ir_program, probe_binding);
        DumpShader(vertex_spv, info.pgm_hash, info.hw_stage, perm_idx, "fixed-vertex.spv");
        ReleaseShader(MakeShader(vertex_spv, {}));
        std::printf("Fixed quad vertex module: %zu words, %u patch inputs\n", vertex_spv.size(),
                    std::popcount(info.uses_patches));
    }
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");
    ListHit(info, spv, runtime_info);

    auto patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
    }
    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    const VideoCore::Backend::ShaderHandle module =
        MakeShader(is_patched ? std::span<const u32>{*patch} : std::span<const u32>{spv}, name);

    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, NativeModule(module), spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    if (cached) {
        RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);
    }
    return module;
}

size_t PipelineCache::NewPermutationIndex(u64 program_hash, size_t in_memory, bool cached) {
    const auto it = cached_permutations.find(program_hash);
    const size_t index = it == cached_permutations.end() ? in_memory : std::max(in_memory, it->second);
    if (cached) {
        cached_permutations[program_hash] = index + 1;
    }
    return index;
}

namespace {
/// BB_SHADER_AHEAD: one thread for the workers' translations, with a stack for the translator
/// (workers have the system's small default), running one at a time for the caller.
class AheadThread {
public:
    AheadThread() {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 * 1024 * 1024);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        pthread_create(&thread, &attr, [](void* self) -> void* {
            static_cast<AheadThread*>(self)->Loop();
            return nullptr;
        }, this);
        pthread_attr_destroy(&attr);
    }
    void Run(const std::function<void()>& job) {
        std::scoped_lock one_caller{caller};
        std::unique_lock lk{mutex};
        current = &job;
        cv.notify_all();
        cv.wait(lk, [this] { return current == nullptr; });
    }

private:
    void Loop() {
        std::unique_lock lk{mutex};
        for (;;) {
            cv.wait(lk, [this] { return current != nullptr; });
            lk.unlock();
            (*current)();
            lk.lock();
            current = nullptr;
            cv.notify_all();
        }
    }
    std::mutex caller, mutex;
    std::condition_variable cv;
    const std::function<void()>* current = nullptr;
};
} // namespace

void PipelineCache::TranslateAhead(HwStage hw_stage, SwStage sw_stage,
                                   const Shader::ShaderParams& params,
                                   Shader::RuntimeInfo runtime_info,
                                   Shader::Backend::Bindings& binding) {
    static const bool on = [] {
        const char* env = std::getenv("BB_SHADER_AHEAD");
        return env && env[0] == '1' && Shader::UsePortableSrtWalker();
    }();
    if (!on) {
        return;
    }
    static AheadThread thread;
    thread.Run([&] {
        const u64 seen_key = HashCombine(
            HashCombine(params.hash, XXH3_64bits(&runtime_info, sizeof(runtime_info))),
            XXH3_64bits(&binding, sizeof(binding)));
        if (const auto it = ahead_seen.find(seen_key); it != ahead_seen.end()) {
            binding = it->second;
            return;
        }
        // The code is the pending draw's, valid until it has run; what the translation reads
        // through pointers may not be, as for the worker's own walk (GetProgram).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            return;
        }
        runtime_fault_recover = &recover;
        Shader::Info info{hw_stage, sw_stage, params};
        auto next = binding;
        const auto ir = Shader::TranslateProgram(params.code, ahead_pools, info, runtime_info, profile);
        auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir, next);
        runtime_fault_recover = nullptr;
        ahead_seen.emplace(seen_key, next);
        binding = next;
        ReleaseShader(MakeShader(spv, GetShaderName(hw_stage, params.hash, 0)));
        if (ShaderLogOn()) {
            std::printf("Shader ahead: %zu translated\n", ahead_seen.size());
        }
    });
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    if (!sel.worker) {
        ListBuild();
    }
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (profile.stage_bindings && sw_stage != SwStage::Compute) {
        binding.EnterStage(u32(sw_stage));
    }
    if (sel.worker) {
        // bbport: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            lk.unlock();
            TranslateAhead(hw_stage, sw_stage, params, runtime_info, binding);
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        sigjmp_buf recover;
        if (sigsetjmp(recover, 0)) {
            worker.failed = true;
            return {};
        }
        runtime_fault_recover = &recover;
        // N2.3: a mesh draw's flattened user data from the engine's draw state, without the walker
        // (BB_RENDER_BOUNDARY=flat); flat-check also fills it and compares (bbport_render_boundary).
        const bool from_state = BbRenderBoundary::WorkerFlat(program, info, hw_stage);
        if (!from_state) {
            info.RefreshFlatBuf();
            BbRenderBoundary::WorkerCheckFlat(program, info, hw_stage);
        }
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        runtime_fault_recover = nullptr;
        const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it == program->modules.end()) {
            worker.failed = true;
            lk.unlock();
            TranslateAhead(hw_stage, sw_stage, params, runtime_info, binding);
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf, from_state});
        return std::make_tuple(&info, it->module, it->spec.fetch_shader_data,
                               HashCombine(params.hash, perm_idx));
    }

    // Motion vertex shaders are never loaded from the cache (LoadShaderMeta): not written either.
    const bool cached = !(hw_stage == HwStage::Vertex && runtime_info.hw.vs.motion_vectors);
    auto it_pgm = program_cache.find(params.hash); // this thread is the only writer
    if (it_pgm == program_cache.end()) {
        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto start = binding;
        const size_t perm_idx = NewPermutationIndex(params.hash, 0, cached);
        const auto compile_start = std::chrono::steady_clock::now();
        const auto module =
            CompileModule(new_program->info, runtime_info, params.code, perm_idx, binding, cached);
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        if (ShaderLogOn()) {
            LogVariant(params.hash, hw_stage, perm_idx,
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compile_start)
                           .count(),
                       spec, nullptr);
        }
        const auto perm_hash = HashCombine(params.hash, perm_idx);

        if (cached) {
            RegisterShaderMeta(new_program->info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        }
        new_program->InsertPermut(module, std::move(spec), perm_idx);
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        return std::make_tuple(&program->info, module,
                               program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
    }

    auto& program = it_pgm.value();
    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        program->info_template = std::make_unique<Shader::Info>(program->info);
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    // N2.4: a compute pass's flattened user data from the engine's compute state
    // (BB_RENDER_BOUNDARY=flat), or the walker, which teaches the boundary the program's slots.
    if (!BbRenderBoundary::GpuComputeFlat(program.get(), info, hw_stage)) {
        info.RefreshFlatBuf();
        BbRenderBoundary::LearnCompute(program.get(), info, hw_stage);
    }
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = program->modules.size();
    u64 perm_hash{};

    VideoCore::Backend::ShaderHandle module{};

    // bbport: consecutive draws of a program almost always use the same permutation.
    auto it = program->last_used < program->modules.size() &&
                      program->modules[program->last_used].spec == spec
                  ? program->modules.begin() + program->last_used
                  : std::ranges::find(program->modules, spec, &Program::Module::spec);
    if (it != program->modules.end()) {
        program->last_used = std::distance(program->modules.begin(), it);
    }
    if (it == program->modules.end()) {
        perm_idx = NewPermutationIndex(params.hash, perm_idx, cached);
        perm_hash = HashCombine(params.hash, perm_idx);
        auto new_info = Shader::Info(hw_stage, sw_stage, params);
        const auto compile_start = std::chrono::steady_clock::now();
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding, cached);
        if (ShaderLogOn()) {
            LogVariant(params.hash, hw_stage, perm_idx,
                       std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compile_start)
                           .count(),
                       spec, program->modules.empty() ? nullptr : &program->modules.front().spec);
        }

        if (cached) {
            RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
        }
        std::unique_lock lk{programs_mutex};
        program->InsertPermut(module, std::move(spec), perm_idx);
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
        perm_hash = HashCombine(params.hash, perm_idx);
    }
    return std::make_tuple(&program->info, module,
                           program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
}

VideoCore::Backend::ShaderHandle PipelineCache::MakeShader(std::span<const u32> spirv,
                                                           std::string_view name) {
    const auto shader = device.CreateShader(spirv, name);
    ASSERT_MSG(shader.handle, "Failed to compile SPIR-V shader {}", name);
    return shader.handle;
}

void PipelineCache::ReleaseShader(VideoCore::Backend::ShaderHandle module) {
    device.DestroyShader(module);
}

vk::ShaderModule PipelineCache::NativeModule(VideoCore::Backend::ShaderHandle module) {
    return module ? vk::ShaderModule{reinterpret_cast<VkShaderModule>(
                        device.NativeHandle(module))}
                  : vk::ShaderModule{};
}

std::optional<VideoCore::Backend::ShaderHandle> PipelineCache::ReplaceShader(
    VideoCore::Backend::ShaderHandle module,
                                                             std::span<const u32> spv_code) {
    std::optional<VideoCore::Backend::ShaderHandle> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                ReleaseShader(m.module);
                m.module = MakeShader(spv_code, {});
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
