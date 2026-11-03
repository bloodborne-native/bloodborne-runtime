// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: lane masks in vertex shaders for Metal, which has no SIMD-group functions in vertex
// functions.

#pragma once

#include <span>
#include <string>
#include <vector>
#include "common/types.h"

namespace Metal {

/// Result of RewriteOneLaneWave.
struct OneLaneRewrite {
    bool ok{};              ///< false: `error` says why; `spirv` is empty
    std::vector<u32> spirv; ///< the rewritten module
    std::string error;
};

/// Whether a module uses subgroup (wave) operations, and whether they are only Ballot and
/// InverseBallot, the pair the translator uses to keep a condition as a lane mask.
struct LaneUse {
    bool subgroup_ops{};
    bool lane_masks_only{true};
};
LaneUse ScanLaneUse(std::span<const u32> spirv);

/// The module as a wave of one lane: OpGroupNonUniformBallot of a condition becomes a mask with
/// bit 0 set to it, OpGroupNonUniformInverseBallot of a mask becomes its bit 0, and the
/// SubgroupLocalInvocationId input (always 0 in such a wave) leaves the interface. A lane that
/// packs its own condition and reads it back gets what it gets in a wider wave. Fails on any
/// other subgroup operation, whose result would depend on the other lanes.
OneLaneRewrite RewriteOneLaneWave(std::span<const u32> spirv);

} // namespace Metal
