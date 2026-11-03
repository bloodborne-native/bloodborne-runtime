// bbport: the pipelines the ahead-of-time compile builds. The game chooses a pipeline's state at
// draw time; the planner knows the engine's rules for it (a model pass's targets, the formats and
// blends of the other passes, the vertex layouts) and adds what this player's sessions have drawn,
// kept in their cache. The pairs of shaders come from the game's files. No backend's objects here:
// the same plan serves any backend. Nothing here is shipped.
#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "video_core/renderer_vulkan/vk_pipeline_key.h"
#include "video_core/renderer_vulkan/vk_shader_scan.h"

namespace Vulkan {

/// One pipeline by its programs (0: no pixel shader). `key`'s stage hashes are not used, except
/// a tessellated draw's: its hull and domain programs (`vertex` is then its local program).
struct AheadPipeline {
    u64 vertex{};
    u64 pixel{};
    GraphicsPipelineKey key{};
};

/// A tessellated draw's programs, as the file names pair them.
struct TessStages {
    u64 local{}, hull{}, domain{};
    auto operator<=>(const TessStages&) const = default;
};
/// Every local, hull and domain program set the game's files give.
std::vector<TessStages> TessellationSets(const ShaderScan& scan);

/// Adds a pipeline a session built or loaded to the player's list, once.
void NotePipeline(const std::filesystem::path& path, const AheadPipeline& pipeline);
/// The player's list; empty when it is missing or from another version.
std::vector<AheadPipeline> ReadNotedPipelines(const std::filesystem::path& path);
/// How many different states `seen` holds.
u32 CountStates(std::span<const AheadPipeline> seen);
/// Every pair the files give, with the rules' states and formats and those `seen` shows.
std::vector<AheadPipeline> PlanPipelines(const ShaderScan& scan, std::span<const AheadPipeline> seen);
/// Prints how many of `seen` the rules alone plan (BB_PIPELINE_PLAN_CHECK).
void CheckPlan(const ShaderScan& scan, std::span<const AheadPipeline> seen);

} // namespace Vulkan
