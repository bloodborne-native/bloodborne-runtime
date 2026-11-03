// SPDX-License-Identifier: GPL-2.0-or-later
// From Wooloomooloo2/bloodborne_mac_port (78b59f5, GPL-2.0-or-later), adapted to bloodborne-runtime.
// bbport: Metal back end "shadow" builder (BB_METAL_SHADOW=1, and always with BB_METAL_FRAME=1) (docs/DESIGN_METAL_BACKEND.md, M1). With BB_METAL_SHADOW=1
// in the arm64 GPU host, every pipeline the Vulkan path creates is also built as a Metal pipeline
// state (renderer_metal/mtl_shadow.mm) and the results are counted; nothing draws with them.
// Everywhere else these are no-ops.

#pragma once

#include <array>
#include <span>
#include <string>
#include <tuple>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
struct GraphicsPipelineKey;
struct ComputePipelineKey;
} // namespace Vulkan

namespace MetalShadow {

/// One vertex attribute as the Vulkan pipeline declares it (GraphicsPipeline::GetVertexInputs).
struct VertexInput {
    u32 location{};
    bool per_instance{};
    u32 divisor{1};
};

#ifdef BB_METAL_BACKEND
bool Enabled();
/// The SPIR-V a Vulkan shader module was made from (CompileSPV).
void RegisterModule(vk::ShaderModule module, std::span<const u32> spirv);
/// The module is gone: its SPIR-V by handle goes (the compiled forms stay with the content
/// cache, which the Metal pipelines point into).
void ForgetModule(vk::ShaderModule module);
/// A graphics pipeline the Vulkan path built; `modules`/`has_stage` are indexed by SwStage.
/// `pipeline`: the Vulkan pipeline, so the Metal frame path finds the Metal one for it.
void Graphics(const Vulkan::GraphicsPipelineKey& key, std::span<const vk::ShaderModule> modules,
              std::span<const bool> has_stage, std::span<const VertexInput> inputs,
              bool aux_fragment, vk::Pipeline pipeline);
void Compute(const Vulkan::ComputePipelineKey& key, vk::ShaderModule module, vk::Pipeline pipeline);
/// Builds a pipeline into Metal's own cache and keeps nothing (the ahead-of-time compile).
/// `why`, when given, receives the reason a pipeline could not be built.
bool Warm(const Vulkan::GraphicsPipelineKey& key, std::span<const vk::ShaderModule> modules,
          std::span<const bool> has_stage, std::span<const VertexInput> inputs,
          std::string* why = nullptr);
/// Around the game pipelines' vkCreate*Pipelines: the layer's generic translation skips them.
struct GameBuildScope {
    GameBuildScope();
    ~GameBuildScope();
};
#else
struct GameBuildScope {};
inline bool Enabled() {
    return false;
}
inline void RegisterModule(vk::ShaderModule, std::span<const u32>) {}
inline void ForgetModule(vk::ShaderModule) {}
inline void Graphics(const Vulkan::GraphicsPipelineKey&, std::span<const vk::ShaderModule>,
                     std::span<const bool>, std::span<const VertexInput>, bool, vk::Pipeline) {}
inline void Compute(const Vulkan::ComputePipelineKey&, vk::ShaderModule, vk::Pipeline) {}
inline bool Warm(const Vulkan::GraphicsPipelineKey&, std::span<const vk::ShaderModule>,
                 std::span<const bool>, std::span<const VertexInput>, std::string* = nullptr) {
    return false;
}
#endif

} // namespace MetalShadow
