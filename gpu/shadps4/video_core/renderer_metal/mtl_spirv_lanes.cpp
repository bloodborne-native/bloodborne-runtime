// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: lane masks in vertex shaders for Metal (mtl_spirv_lanes.h).

#include "video_core/renderer_metal/mtl_spirv_lanes.h"

#include <fmt/format.h>

namespace Metal {

namespace {

constexpr u32 kMagic = 0x07230203;
constexpr u32 kHeaderWords = 5;

constexpr u32 OpName = 5;
constexpr u32 OpEntryPoint = 15;
constexpr u32 OpCapability = 17;
constexpr u32 OpTypeInt = 21;
constexpr u32 OpConstant = 43;
constexpr u32 OpFunction = 54;
constexpr u32 OpVariable = 59;
constexpr u32 OpLoad = 61;
constexpr u32 OpDecorate = 71;
constexpr u32 OpCompositeConstruct = 80;
constexpr u32 OpCompositeExtract = 81;
constexpr u32 OpCopyObject = 83;
constexpr u32 OpSelect = 169;
constexpr u32 OpINotEqual = 171;
constexpr u32 OpBitwiseAnd = 199;
constexpr u32 OpGroupNonUniformFirst = 333; // OpGroupNonUniformElect
constexpr u32 OpGroupNonUniformLast = 366;  // OpGroupNonUniformQuadSwap
constexpr u32 OpGroupNonUniformBallot = 339;
constexpr u32 OpGroupNonUniformInverseBallot = 340;

constexpr u32 CapabilityGroupNonUniform = 61;
constexpr u32 CapabilityGroupNonUniformBallot = 64;
constexpr u32 DecorationBuiltIn = 11;
constexpr u32 BuiltInSubgroupLocalInvocationId = 41;

u32 Op(u32 word) {
    return word & 0xffff;
}
u32 Words(u32 word) {
    return word >> 16;
}
u32 Head(u32 op, u32 words) {
    return words << 16 | op;
}

} // namespace

LaneUse ScanLaneUse(std::span<const u32> spirv) {
    LaneUse use;
    for (size_t i = kHeaderWords; i < spirv.size() && Words(spirv[i]) != 0; i += Words(spirv[i])) {
        const u32 op = Op(spirv[i]);
        if (op >= OpGroupNonUniformFirst && op <= OpGroupNonUniformLast) {
            use.subgroup_ops = true;
            use.lane_masks_only = use.lane_masks_only &&
                                  (op == OpGroupNonUniformBallot || op == OpGroupNonUniformInverseBallot);
        }
    }
    return use;
}

OneLaneRewrite RewriteOneLaneWave(std::span<const u32> spirv) {
    OneLaneRewrite out;
    if (spirv.size() < kHeaderWords || spirv[0] != kMagic) {
        out.error = "not a SPIR-V module";
        return out;
    }
    // First pass: the 32-bit unsigned type and the lane index input.
    u32 uint_type = 0, lane_id = 0;
    for (size_t i = kHeaderWords; i < spirv.size();) {
        const u32 words = Words(spirv[i]), op = Op(spirv[i]);
        if (words == 0 || i + words > spirv.size()) {
            out.error = "a malformed instruction";
            return out;
        }
        if (op == OpTypeInt && words == 4 && spirv[i + 2] == 32 && spirv[i + 3] == 0) {
            uint_type = spirv[i + 1];
        } else if (op == OpDecorate && words >= 4 && spirv[i + 2] == DecorationBuiltIn &&
                   spirv[i + 3] == BuiltInSubgroupLocalInvocationId) {
            lane_id = spirv[i + 1];
        } else if (op >= OpGroupNonUniformFirst && op <= OpGroupNonUniformLast &&
                   op != OpGroupNonUniformBallot && op != OpGroupNonUniformInverseBallot) {
            out.error = fmt::format("subgroup operation {} depends on the other lanes", op);
            return out;
        }
        i += words;
    }
    if (uint_type == 0) {
        out.error = "no 32-bit unsigned type";
        return out;
    }
    u32 bound = spirv[3];
    const u32 zero = bound++, one = bound++;
    std::vector<u32>& code = out.spirv;
    code.assign(spirv.begin(), spirv.begin() + kHeaderWords);
    bool constants_placed = false;
    for (size_t i = kHeaderWords; i < spirv.size();) {
        const u32 words = Words(spirv[i]), op = Op(spirv[i]);
        const std::span<const u32> in = spirv.subspan(i, words);
        i += words;
        if (op == OpCapability && (in[1] == CapabilityGroupNonUniform || in[1] == CapabilityGroupNonUniformBallot)) {
            continue;
        }
        if (lane_id != 0 && (op == OpName || op == OpDecorate) && in[1] == lane_id) {
            continue;
        }
        if (lane_id != 0 && op == OpVariable && in[2] == lane_id) {
            continue;
        }
        if (op == OpEntryPoint && lane_id != 0) {
            // Execution model, function, a nul-terminated name, then the interface.
            size_t at = 3;
            while (at < in.size() && (in[at] >> 24) != 0) {
                ++at;
            }
            ++at;
            const size_t start = code.size();
            code.insert(code.end(), in.begin(), in.begin() + std::min(at, in.size()));
            for (; at < in.size(); ++at) {
                if (in[at] != lane_id) {
                    code.push_back(in[at]);
                }
            }
            code[start] = Head(OpEntryPoint, u32(code.size() - start));
            continue;
        }
        if (op == OpFunction && !constants_placed) {
            code.insert(code.end(), {Head(OpConstant, 4), uint_type, zero, 0u});
            code.insert(code.end(), {Head(OpConstant, 4), uint_type, one, 1u});
            constants_placed = true;
        }
        if (lane_id != 0 && op == OpLoad && words >= 4 && in[3] == lane_id) {
            code.insert(code.end(), {Head(OpCopyObject, 4), in[1], in[2], zero}); // lane 0
            continue;
        }
        if (op == OpGroupNonUniformBallot && words == 5) {
            // The mask of a one-lane wave: bit 0 is the lane's condition.
            const u32 bit = bound++;
            code.insert(code.end(), {Head(OpSelect, 6), uint_type, bit, in[4], one, zero});
            code.insert(code.end(), {Head(OpCompositeConstruct, 7), in[1], in[2], bit, zero, zero, zero});
            continue;
        }
        if (op == OpGroupNonUniformInverseBallot && words == 5) {
            // Lane 0's bit of the mask.
            const u32 low = bound++, masked = bound++;
            code.insert(code.end(), {Head(OpCompositeExtract, 5), uint_type, low, in[4], 0u});
            code.insert(code.end(), {Head(OpBitwiseAnd, 5), uint_type, masked, low, one});
            code.insert(code.end(), {Head(OpINotEqual, 5), in[1], in[2], masked, zero});
            continue;
        }
        code.insert(code.end(), in.begin(), in.end());
    }
    if (!constants_placed) {
        out.spirv.clear();
        out.error = "no function";
        return out;
    }
    code[3] = bound;
    out.ok = true;
    return out;
}

} // namespace Metal
