// bbport: the pipelines the ahead-of-time compile builds. The game chooses a pipeline's state at
// draw time, so the states come from what this player's sessions have drawn, kept in their cache;
// the pairs of shaders come from the game's files. Nothing here is shipped.
#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "video_core/renderer_vulkan/vk_pipeline_key.h"
#include "video_core/renderer_vulkan/vk_shader_scan.h"

namespace Vulkan {

/// One pipeline by its programs (0: no pixel shader). `key`'s stage hashes are not used.
struct AheadPipeline {
    u64 vertex{};
    u64 pixel{};
    GraphicsPipelineKey key{};
};

/// Adds a pipeline a session built or loaded to the player's list, once.
void NotePipeline(const std::filesystem::path& path, const AheadPipeline& pipeline);
/// The player's list; empty when it is missing or from another version.
std::vector<AheadPipeline> ReadNotedPipelines(const std::filesystem::path& path);
/// How many different states `seen` holds.
u32 CountStates(std::span<const AheadPipeline> seen);
/// Every pair the files give, with the states and vertex formats `seen` shows for its kind.
std::vector<AheadPipeline> PlanPipelines(const ShaderScan& scan, std::span<const AheadPipeline> seen);

} // namespace Vulkan
