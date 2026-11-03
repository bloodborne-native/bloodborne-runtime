// bbport: see vk_pipeline_ahead.h.
#include "video_core/renderer_vulkan/vk_pipeline_ahead.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <xxhash.h>

#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace Vulkan {
namespace {

constexpr size_t FormatsStart = offsetof(GraphicsPipelineKey, vertex_buffer_formats);
constexpr size_t StateStart = offsetof(GraphicsPipelineKey, patch_control_points);
constexpr u32 StatesPerKind = 2; ///< a pair is built with its kind's most used states
constexpr u32 FormatSets = 3;    ///< vertex format sets tried for a program without semantics

std::string StateOf(const GraphicsPipelineKey& key) {
    const auto* bytes = reinterpret_cast<const char*>(&key);
    return {bytes + StateStart, sizeof(key) - StateStart};
}

using Counts = std::unordered_map<std::string, u32>;

std::vector<std::string> Top(const Counts& counts, u32 most) {
    std::vector<std::pair<std::string, u32>> sorted{counts.begin(), counts.end()};
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::vector<std::string> out;
    for (u32 i = 0; i < sorted.size() && i < most; ++i) {
        out.push_back(sorted[i].first);
    }
    return out;
}

// The model shaders' render passes: file names end with the pass.
constexpr std::array<std::string_view, 21> Passes{
    "fwd",  "velo", "dpt",      "dpta",  "dptv",      "dptva", "dptvcube",
    "dptvacube", "fwdsdw", "fwdssdw", "lpp0", "lpp1", "lpp1ssdw", "lpp1f",
    "lpp1ssdwf", "gbuf", "gbuf2", "pnt", "pntcube", "so", "watdpt"};

/// A pixel pass's vertex pass: some pixel passes share another's vertex shader.
std::string_view VertexPass(size_t pass) {
    return Passes[pass == 9 ? 0 : pass == 14 ? 12 : pass == 13 ? 11 : pass];
}

/// A name's pass, for grouping states: its token, else the text before the first separator.
std::string PassOf(std::string_view name) {
    static constexpr std::array<std::string_view, 11> tokens{
        "gbuf2", "gbuf", "dptvcube", "dptva", "dptv", "dpta", "dpt", "fwdsdw", "fwd", "velo", "df"};
    for (size_t at = name.find('_'); at != name.npos; at = name.find('_', at + 1)) {
        for (const auto token : tokens) {
            const auto rest = name.substr(at + 1);
            if (rest.starts_with(token) && rest.size() > token.size() &&
                (rest[token.size()] == '_' || rest[token.size()] == '@' ||
                 rest[token.size()] == '.')) {
                return std::string{token};
            }
        }
    }
    return std::string{name.substr(0, name.find_first_of("_.0123456789"))};
}

std::string Group(const ScannedShader& pixel) {
    return pixel.executable ? "exe" : pixel.bundle;
}
std::string FineKind(const ScannedShader& pixel) {
    return fmt::format("{}|{}|{:x}|{:x}|{:x}", Group(pixel), pixel.executable ? "" : PassOf(pixel.name),
                       pixel.colour, pixel.word15, pixel.word13);
}
std::string WideKind(const ScannedShader& pixel) {
    return fmt::format("{}|{:x}", Group(pixel), pixel.colour);
}

// The states the engine draws with, by pass. A model pass's targets are fixed by the renderer; its
// blending comes from the material. The other programs are full-screen, effect and interface
// passes: their targets are among the engine's few formats. Values as GraphicsPipelineKey holds them.
constexpr u64 Rgba8 = 0x000706050400800a, Bgra8 = 0x000704050600800a, Srgb8 = 0x000706050400824a,
              R11G11B10 = 0x00000605040081c6, Rgba16f = 0x00070605040081cc,
              Rg16f = 0x00000005040081c5, R32f = 0x00000000040121c4;
constexpr u32 NoBlend = 0, Additive = 0x60010104, Alpha = 0x60010504;
constexpr u32 DepthStencil = 0x0f, DepthOnly = 0x0b, DepthNoClamp = 0x09, NoDepth = 0x08;
constexpr u32 SourceOver = 0x40000504, SourceAdd = 0x40000104, SourceOverColour = 0x40000508;
constexpr u32 TriangleList = 0x2c4, TriangleStrip = 0x2c6, RectList = 0x251, RectListNoFill = 0x2d1, StripNoClip = 0xc6,
              ListNoClip = 0x244;

/// Whether a state (as MakeState makes it) draws rect lists: its topology is the key's last word.
bool IsRectList(const std::string& state) {
    u32 topology = 0;
    std::memcpy(&topology, state.data() + state.size() - 4, 4);
    return topology == RectList || topology == RectListNoFill;
}

constexpr u32 Patches = 0x09; ///< the topology word's primitive byte for a tessellated draw

/// Whether a state has a depth target: the depth word's low bits are its format.
bool HasDepth(const std::string& state) {
    u32 z = 0;
    std::memcpy(&z, state.data() + state.size() - 8, 4);
    return (z & 3) != 0;
}

/// Whether a pixel program writes depth (DB_SHADER_CONTROL's Z_EXPORT_ENABLE, in its header).
bool WritesDepth(const ScannedShader& pixel) {
    return pixel.kind == 2 && (pixel.depth_control & 1) != 0;
}

/// The topology word's low five bits are the primitive (polygon mode and clip space follow).
bool IsPatches(const std::string& state) {
    return (u8(state[state.size() - 4]) & 0x1f) == Patches;
}

/// A state drawn as patches of `points` control points, as the engine draws tessellated effects.
std::string AsPatches(std::string state, u32 points) {
    auto& primitive = state[state.size() - 4];
    primitive = char((u8(primitive) & ~0x1f) | Patches);
    std::memcpy(state.data(), &points, 4); // the state starts with the patch's control points
    return state;
}

/// One state: `colours` empty for a pass that writes only depth.
std::string MakeState(std::span<const u64> colours, u32 blend, u32 write_mask, u32 z, u32 topology) {
    GraphicsPipelineKey key{};
    const u32 count = std::max<u32>(u32(colours.size()), 1);
    key.num_color_attachments = count;
    std::memcpy(key.color_buffers.data(), colours.data(), colours.size() * sizeof(u64));
    auto* bytes = reinterpret_cast<char*>(&key);
    for (u32 i = 0; i < colours.size(); ++i) {
        std::memcpy(bytes + offsetof(GraphicsPipelineKey, blend_controls) + i * 4, &blend, 4);
        std::memcpy(bytes + offsetof(GraphicsPipelineKey, write_masks) + i * 4, &write_mask, 4);
        key.color_samples[i] = 1;
    }
    const u32 mask = count == 8 ? ~0u : (1u << (count * 4)) - 1;
    std::memcpy(bytes + offsetof(GraphicsPipelineKey, cb_shader_mask), &mask, 4);
    std::memset(bytes + offsetof(GraphicsPipelineKey, logic_op), 0xcc, 1);
    key.num_samples = key.depth_samples = 1;
    key.mrt_mask = (1u << count) - 1;
    std::memcpy(bytes + sizeof(key) - 8, &z, 4);
    std::memcpy(bytes + sizeof(key) - 4, &topology, 4);
    return StateOf(key);
}

/// A state with no colour target at all: a pixel shader that writes only depth.
std::string NoTargetState(u32 z, u32 topology) {
    GraphicsPipelineKey key{};
    auto* bytes = reinterpret_cast<char*>(&key);
    std::memset(bytes + offsetof(GraphicsPipelineKey, logic_op), 0xcc, 1);
    key.num_samples = key.depth_samples = 1;
    std::memcpy(bytes + sizeof(key) - 8, &z, 4);
    std::memcpy(bytes + sizeof(key) - 4, &topology, 4);
    return StateOf(key);
}

/// One state with each target's own blend and write mask (`blends` and `masks` as long as `colours`).
std::string MakeStatePerTarget(std::span<const u64> colours, std::span<const u32> blends,
                               std::span<const u32> masks, u32 z, u32 topology) {
    auto state = MakeState(colours, NoBlend, 0, z, topology);
    for (u32 i = 0; i < colours.size(); ++i) {
        std::memcpy(state.data() + offsetof(GraphicsPipelineKey, blend_controls) - StateStart + i * 4,
                    &blends[i], 4);
        std::memcpy(state.data() + offsetof(GraphicsPipelineKey, write_masks) - StateStart + i * 4,
                    &masks[i], 4);
    }
    return state;
}

// The engine's material blends (GXGraphics::createCommonStates, Class_53b8d30::applyBlendState in
// the recompiler's annotations): five modes (opaque, alpha, multiply, add, subtract), in groups
// that a pass's draw flags choose (SceneRenderer::setupPassDrawItem). The words are the blend
// registers those states encode to; alpha to coverage changes no pipeline.
constexpr std::array<u32, 4> BlendModes{0x60010504, 0x60010008, 0x60010104, 0x60010184};

/// A model pass's states; a pass with a colour word of `targets` entries.
void ModelStates(std::string_view pass, u32 targets, std::set<std::string>& out) {
    if (pass.starts_with("dptv")) {
        const std::array colours{Rg16f};
        out.insert(MakeState(colours, NoBlend, 0xf, DepthOnly, TriangleList));
    } else if (pass.starts_with("dpt") || pass == "so" || pass == "watdpt") {
        for (const u32 z : {DepthStencil, DepthOnly, DepthNoClamp}) {
            out.insert(MakeState({}, NoBlend, 0, z, TriangleList));
        }
    } else if (pass.starts_with("gbuf")) {
        constexpr std::array colours{Rgba8, Rgba8, Srgb8, Rgba8, R11G11B10, Rgba16f, Rgba16f};
        const std::span used{colours.data(), std::min<size_t>(targets, colours.size())};
        if (pass.starts_with("gbuf2")) {
            // Gbuf2 (group 4): the first target is never written, the others RGB when blended.
            std::vector<u32> blends(used.size()), masks(used.size(), 0xf);
            masks[0] = 0;
            out.insert(MakeStatePerTarget(used, blends, masks, DepthStencil, TriangleList));
            for (const u32 mode : BlendModes) {
                std::fill(blends.begin(), blends.end(), mode);
                std::fill(masks.begin() + 1, masks.end(), 0x7);
                out.insert(MakeStatePerTarget(used, blends, masks, DepthStencil, TriangleList));
            }
        } else {
            // Gbuf (group 3): opaque writes everything, blended modes RGB only.
            out.insert(MakeState(used, NoBlend, 0xf, DepthStencil, TriangleList));
            for (const u32 mode : BlendModes) {
                out.insert(MakeState(used, mode, 0x7, DepthStencil, TriangleList));
            }
        }
    } else if (pass.starts_with("pnt")) {
        // Point lights: the light renderer's fixed blends (material blends are skipped).
        const std::array colours{Srgb8, Rgba8};
        for (const u32 blend : {0x65000504u, 0x65000008u}) {
            out.insert(MakeState(colours, blend, 0xf, DepthStencil, TriangleList));
        }
    } else if (pass == "velo") {
        const std::array colours{Rgba8};
        out.insert(MakeState(colours, NoBlend, 0xf, DepthStencil, TriangleList));
        for (const u32 mode : BlendModes) {
            out.insert(MakeState(colours, mode, 0xf, DepthStencil, TriangleList));
        }
    } else { // forward and light passes: the material's mode, group 0 (Lpp0: RGB only)
        const std::array colours{Rgba16f};
        const u32 mask = pass == "lpp0" ? 0x7 : 0xf;
        out.insert(MakeState(colours, NoBlend, 0xf, DepthStencil, TriangleList));
        for (const u32 mode : BlendModes) {
            out.insert(MakeState(colours, mode, mask, DepthStencil, TriangleList));
        }
    }
}

/// A full-screen, effect or interface program's states.
void FixedStates(const ScannedShader& pixel, std::set<std::string>& out) {
    u32 targets = 0;
    for (u32 colour = pixel.colour; colour != 0; colour >>= 4) {
        targets += (colour & 0xf) != 0;
    }
    static constexpr std::array one{Rgba8, Bgra8, Rgba16f, R11G11B10, Rg16f, R32f, Srgb8};
    for (const u32 topology : {TriangleStrip, RectList, StripNoClip, ListNoClip}) {
        for (const u32 z : {NoDepth, DepthStencil}) {
            out.insert(MakeState({}, NoBlend, 0, z, topology));
            if (targets == 0) {
                out.insert(NoTargetState(z, topology));
            }
            if (targets <= 1) {
                for (const u64 colour : one) {
                    const std::array colours{colour};
                    for (const u32 mask : {0xfu, 0x7u}) {
                        out.insert(MakeState(colours, NoBlend, mask, z, topology));
                    }
                    for (const u32 blend : {Additive, Alpha}) {
                        out.insert(MakeState(colours, blend, 0xf, z, topology));
                    }
                }
            } else if (targets == 2) {
                const std::array same{R11G11B10, R11G11B10}, hdr{Rgba16f, Rgba16f};
                for (const u32 blend : {NoBlend, Additive}) {
                    out.insert(MakeState(same, blend, 0xf, z, topology));
                    out.insert(MakeState(hdr, blend, 0xf, z, topology));
                }
                out.insert(MakeState(same, NoBlend, 0x7, z, topology));
            }
        }
    }
}

/// The executable holds the YEBIS effects and, after a wide gap, the engine's own programs. Returns
/// where those start. YEBIS: full-screen with one target, none seen drawn with anything else.
size_t EngineProgramsStart(const ShaderScan& scan) {
    std::vector<size_t> positions;
    for (const auto& shader : scan.shaders) {
        if (shader.executable) {
            positions.push_back(shader.position);
        }
    }
    std::sort(positions.begin(), positions.end());
    size_t gap = 0, start = 0;
    for (size_t i = positions.size() / 2; i < positions.size(); ++i) {
        if (positions[i] - positions[i - 1] > gap) {
            gap = positions[i] - positions[i - 1];
            start = positions[i];
        }
    }
    return start;
}

/// The engine's programs in the executable: 2D drawing, full-screen passes, depth only.
void EngineStates(std::set<std::string>& out) {
    const std::array bgra{Bgra8};
    for (const u32 blend : {NoBlend, SourceOver, SourceAdd, SourceOverColour}) {
        out.insert(MakeState(bgra, blend, 0xf, DepthStencil, ListNoClip));
    }
    for (const u64 colour : {Bgra8, Rgba8, Rgba16f, R11G11B10}) {
        const std::array colours{colour};
        for (const u32 z : {NoDepth, DepthStencil}) {
            for (const u32 mask : {0xfu, 0x8u}) {
                // Rect lists only reach programs without attributes (PlanPipelines).
                for (const u32 topology : {StripNoClip, RectList}) {
                    out.insert(MakeState(colours, NoBlend, mask, z, topology));
                }
            }
        }
    }
    for (const u32 topology : {ListNoClip, RectList, RectListNoFill}) {
        out.insert(MakeState({}, NoBlend, 0, DepthStencil, topology));
    }
}

/// YEBIS passes: full-screen, one target, drawn with depth testing off or on.
void EffectStates(std::set<std::string>& out) {
    for (const u32 z : {NoDepth, DepthStencil}) {
        for (const u64 colour : {Rgba16f, R11G11B10, Bgra8, Rgba8, Rg16f}) {
            const std::array colours{colour};
            out.insert(MakeState(colours, NoBlend, 0xf, z, StripNoClip));
        }
        const std::array hdr{Rgba16f};
        out.insert(MakeState(hdr, NoBlend, 0x8, z, StripNoClip));
    }
}

/// The particle and effect bundle: blended over the scene, or one of two targets.
void ParticleStates(std::set<std::string>& out) {
    for (const u32 topology : {TriangleList, TriangleStrip}) {
        for (const u32 z : {DepthStencil, NoDepth}) {
            for (const u32 blend : {NoBlend, Additive, Alpha}) {
                for (const u64 colour : {Rgba16f, Rgba8}) {
                    const std::array colours{colour};
                    out.insert(MakeState(colours, blend, 0xf, z, topology));
                }
                const std::array two{Rgba16f, Rgba16f};
                out.insert(MakeState(two, blend, 0xf, z, topology));
            }
        }
    }
}

/// The model pairs by file name: a pixel shader with its pass's vertex shaders.
void ModelPairs(const ShaderScan& scan, std::set<std::pair<u64, u64>>& pairs) {
    static constexpr std::array<std::string_view, 4> skins{"", "_skin", "_morphskin", "_morph"};
    static constexpr std::array<std::string_view, 2> instanced{"", "_ins"};
    static constexpr std::array<std::string_view, 2> clip{"", "@[cl]"};
    for (const auto& [file, hash] : scan.model_files) {
        std::string_view name{file};
        if (!name.ends_with(".ppo")) {
            continue;
        }
        name.remove_suffix(4);
        if (name.ends_with("]")) {
            const size_t bracket = name.rfind("_[");
            if (bracket != name.npos) {
                name = name.substr(0, bracket);
            }
        }
        const size_t split = name.rfind('_');
        if (split == name.npos) {
            continue;
        }
        const auto pass = std::find(Passes.begin(), Passes.end(), name.substr(split + 1));
        if (pass == Passes.end()) {
            continue;
        }
        const std::string stem{name.substr(0, split)};
        const auto vertex = VertexPass(size_t(pass - Passes.begin()));
        for (const auto skin : skins) {
            for (const auto ins : instanced) {
                for (const auto cl : clip) {
                    const auto it = scan.model_files.find(
                        fmt::format("{}_{}{}{}{}.vpo", stem, vertex, skin, ins, cl));
                    if (it != scan.model_files.end()) {
                        pairs.insert({it->second, hash});
                    }
                }
            }
        }
    }
}

/// The other pairs: within a bundle, or among the executable's, a vertex shader that exports what
/// the pixel shader reads. Bundles of full-screen and interface passes, and the YEBIS effects
/// (the executable's programs before `engine_programs`), also pair on a subset.
void ExportPairs(const ShaderScan& scan, size_t engine_programs, std::set<std::pair<u64, u64>>& pairs) {
    using Set = std::set<u8>;
    std::map<std::pair<std::string, Set>, std::vector<u64>> exact;
    std::map<std::string, std::vector<const ScannedShader*>> vertex_by_group;
    for (const auto& shader : scan.shaders) {
        if (shader.kind == 1 && !shader.domain && shader.bundle != "gxflvershader") {
            const auto group = Group(shader);
            exact[{group, Set(shader.exports.begin(), shader.exports.end())}].push_back(shader.hash);
            vertex_by_group[group].push_back(&shader);
        }
    }
    for (const auto& shader : scan.shaders) {
        if (shader.kind != 2 || shader.bundle == "gxflvershader") {
            continue;
        }
        const auto group = Group(shader);
        const Set inputs(shader.pixel_inputs.begin(), shader.pixel_inputs.end());
        const auto it = exact.find({group, inputs});
        if (it != exact.end()) {
            for (const u64 vertex : it->second) {
                pairs.insert({vertex, shader.hash});
            }
        }
        const auto effect = [engine_programs](const ScannedShader& program) {
            return program.executable && program.position < engine_programs;
        };
        if (effect(shader)) {
            // A YEBIS pixel program without an exact match: the vertex programs exporting the
            // fewest outputs beyond what it reads.
            if (it != exact.end()) {
                continue;
            }
            size_t fewest = ~size_t{0};
            std::vector<u64> nearest;
            for (const auto* vertex : vertex_by_group[group]) {
                const Set exports(vertex->exports.begin(), vertex->exports.end());
                if (!effect(*vertex) ||
                    !std::includes(exports.begin(), exports.end(), inputs.begin(), inputs.end())) {
                    continue;
                }
                if (exports.size() < fewest) {
                    fewest = exports.size();
                    nearest.clear();
                }
                if (exports.size() == fewest) {
                    nearest.push_back(vertex->hash);
                }
            }
            for (const u64 vertex : nearest) {
                pairs.insert({vertex, shader.hash});
            }
        } else if (group == "gxrenderershader" || group == "gxposteffect" || group == "gxshader" ||
                   group == "gxgui" || group == "gxffxshader") {
            for (const auto* vertex : vertex_by_group[group]) {
                const Set exports(vertex->exports.begin(), vertex->exports.end());
                if (std::includes(exports.begin(), exports.end(), inputs.begin(), inputs.end())) {
                    pairs.insert({vertex->hash, shader.hash});
                }
            }
        }
    }
}

using Formats = std::vector<u32>;

/// What this player's sessions show of vertex formats.
struct FormatTable {
    std::map<std::pair<std::string, u32>, Counts> by_semantic;
    std::map<u32, Counts> by_size;

    /// The engine's vertex layouts, most used first; what a session draws adds to them.
    FormatTable() {
        const auto rule = [this](std::string_view semantic, u32 size, std::initializer_list<u32> formats) {
            u32 weight = u32(formats.size());
            for (const u32 format : formats) {
                by_semantic[{std::string{semantic}, size}][fmt::format("{}", format)] += weight--;
            }
        };
        // A shader reads up to four components of any buffer: without a semantic, its size only hints.
        const auto sized = [this](u32 size, std::initializer_list<u32> formats) {
            u32 weight = u32(formats.size());
            for (const u32 format : formats) {
                by_size[size][fmt::format("{}", format)] += weight--;
            }
        };
        constexpr u32 Bytes4 = 37, Packed = 41, Half2 = 82, Half4 = 96, Float2 = 103, Float3 = 106,
                      Float4 = 109;
        sized(1, {100, Packed, 14});
        sized(2, {Float2, Half4, Half2});
        sized(3, {Float3, Float2, Float4, Packed, Bytes4});
        sized(4, {Bytes4, Float2, Float4, Packed, Half4});
        rule("POSITION", 3, {Float3, Float4});
        rule("POSITION", 4, {Float4});
        rule("NORMAL", 3, {Packed});
        rule("NORMAL", 4, {Packed});
        rule("TANGENT", 3, {Packed});
        rule("TANGENT", 4, {Packed});
        rule("BINORMAL", 1, {Packed});
        rule("BLENDINDICES", 4, {Packed});
        rule("BLENDWEIGHT", 4, {Bytes4});
        rule("COLOR", 3, {Bytes4});
        rule("COLOR", 4, {Bytes4});
        rule("TEXCOORD", 2, {Float2, Half2, Half4});
        rule("TEXCOORD", 3, {Bytes4});
        rule("TEXCOORD", 4, {Half4, Float4});
        // Scaleform's inputs, by name: its mesh formats hold positions as two floats or two
        // 16-bit integers, colours and factors as four bytes, texture coordinates as two floats;
        // the batch index one signed byte, or four bytes beside a colour.
        constexpr u32 Short2 = 82, Byte1 = 14;
        for (const u32 size : {1u, 2u, 3u, 4u}) {
            rule("pos", size, {Float2, Short2});
            rule("acolor", size, {Bytes4});
            rule("afactor", size, {Bytes4});
            rule("atc", size, {Float2});
            rule("vbatch", size, {Byte1, Bytes4});
        }
    }

    static void Add(Counts& counts, u32 format) {
        ++counts[fmt::format("{}", format)];
    }
    void Learn(const ScannedShader& vertex, const GraphicsPipelineKey& key) {
        for (size_t i = 0; i < vertex.attribute_sizes.size() && i < MaxVertexBufferCount; ++i) {
            const u32 format = u32(key.vertex_buffer_formats[i]);
            if (format == 0) {
                continue;
            }
            if (vertex.semantics.size() == vertex.attribute_sizes.size()) {
                Add(by_semantic[{vertex.semantics[i], vertex.attribute_sizes[i]}], format);
            }
            Add(by_size[vertex.attribute_sizes[i]], format);
        }
    }
    std::vector<Formats> For(const ScannedShader& vertex) const {
        const size_t count = vertex.attribute_sizes.size();
        if (count == 0) {
            return {Formats{}};
        }
        std::vector<Formats> out;
        const auto add = [&out](Formats formats) {
            if (std::find(out.begin(), out.end(), formats) == out.end()) {
                out.push_back(std::move(formats));
            }
        };
        const auto pick = [](const Counts& counts, u32 rank) -> u32 {
            const auto top = Top(counts, rank + 1);
            return top.empty() ? 0 : u32(std::stoul(top[std::min<size_t>(rank, top.size() - 1)]));
        };
        if (vertex.semantics.size() == count) {
            Formats named(count);
            bool known = true;
            for (size_t i = 0; i < count; ++i) {
                const auto it = by_semantic.find({vertex.semantics[i], vertex.attribute_sizes[i]});
                known = known && it != by_semantic.end();
                named[i] = it != by_semantic.end() ? pick(it->second, 0) : 0;
            }
            if (known) {
                add(named);
                // Meshes differ most in their texture coordinates and position.
                for (size_t i = 0; i < count; ++i) {
                    if (vertex.semantics[i] != "TEXCOORD" && vertex.semantics[i] != "POSITION" &&
                        vertex.semantics[i] != "pos" && vertex.semantics[i] != "vbatch") {
                        continue;
                    }
                    const auto& counts = by_semantic.at({vertex.semantics[i], vertex.attribute_sizes[i]});
                    for (u32 rank = 1; rank < counts.size(); ++rank) {
                        auto other = named;
                        other[i] = pick(counts, rank);
                        add(other);
                        for (size_t j = i + 1; j < count; ++j) {
                            if (vertex.semantics[j] == "TEXCOORD") {
                                other[j] = pick(by_semantic.at({"TEXCOORD", vertex.attribute_sizes[j]}),
                                                rank);
                            }
                        }
                        add(std::move(other));
                    }
                }
                return out;
            }
        }
        // Without semantics: the attributes' likely formats crossed, for short layouts.
        if (count <= 3) {
            constexpr u32 tried = 3;
            std::vector<Formats> crossed{Formats{}};
            for (size_t i = 0; i < count; ++i) {
                const auto it = by_size.find(vertex.attribute_sizes[i]);
                if (it == by_size.end()) {
                    return {};
                }
                std::vector<Formats> next;
                for (const auto& partial : crossed) {
                    for (u32 rank = 0; rank < std::min<u32>(tried, u32(it->second.size())); ++rank) {
                        auto longer = partial;
                        longer.push_back(pick(it->second, rank));
                        next.push_back(std::move(longer));
                    }
                }
                crossed = std::move(next);
            }
            for (auto& formats : crossed) {
                add(std::move(formats));
            }
            return out;
        }
        for (u32 rank = 0; rank < FormatSets; ++rank) {
            Formats plain(count);
            bool known = true;
            for (size_t i = 0; i < count; ++i) {
                const auto it = by_size.find(vertex.attribute_sizes[i]);
                known = known && it != by_size.end();
                plain[i] = it != by_size.end() ? pick(it->second, rank) : 0;
            }
            if (known) {
                add(std::move(plain));
            }
        }
        return out;
    }
};

/// The engine's fixed vertex layouts, by vertex program (file name without ".vpo"): the input
/// element arrays its renderers create those programs with (GXSimple, GUI, screen quads, and the
/// effects renderer's tables for the GXFfx and VFX programs), as Vulkan formats in attribute
/// order. A layout applies where the program reads every element.
const std::map<std::string, Formats, std::less<>>& EngineLayouts() {
    constexpr u32 Bytes4 = 37, Unorm16x4 = 91, Ushort4 = 95, Half4 = 97, Float1 = 100, Uint2 = 101,
                  Float2 = 103, Float3 = 106, Uint4 = 107, Float4 = 109;
    static const std::map<std::string, Formats, std::less<>> layouts{
        {"gxsimple_v", {Float4, Float4}},
        {"gxsimple_transform_v", {Float4, Float4}},
        {"gxsimple_single2dtexture_v", {Float4, Float4, Float2}},
        {"gxsimple_transformsingle2dtexture_v", {Float4, Float4, Float2}},
        {"gxsimple_skybox_v", {Float4}},
        {"gxsimple_debugfont_v", {Float2, Float3, Bytes4}},
        {"gxguishader_tex1", {Float3, Bytes4, Float3}},
        {"gxguishader_tex2", {Float3, Bytes4, Float2, Float2}},
        {"gxguishader_color2", {Float3, Bytes4, Bytes4, Float2}},
        {"screenspacequadshader", {Float4, Float2}},
        {"screenspacequadshaderz", {Float4, Float2}},
        {"gxffxbumptexture", {Float3, Bytes4, Bytes4, Bytes4, Bytes4, Float2, Float1, Float1, Float2, Float1}},
        {"gxffxcoloronly", {Float3, Bytes4}},
        {"gxffxdistortion", {Float3, Float2}},
        {"gxffxlightshaft", {Float4, Float4, Float4, Float4, Float4}},
        {"gxffxmultitexture", {Float3, Bytes4, Float4, Float2, Float2}},
        {"gxffxpointsprite", {Float4, Bytes4}},
        {"gxffxradialblur", {Float3, Float2, Float2}},
        {"gxffxsoftbumptexture", {Float3, Bytes4, Bytes4, Bytes4, Bytes4, Float2, Float1, Float1, Float2, Float1}},
        {"gxffxsoftmultitexture", {Float3, Bytes4, Float4, Float2, Float2}},
        {"gxffxsofttexture", {Float3, Bytes4, Bytes4, Bytes4, Bytes4, Float2, Float1, Float1, Float2, Float1}},
        {"gxffxtessellatebumptexture", {Float3, Bytes4, Float4, Unorm16x4, Float2, Float4, Float4, Float4}},
        {"gxffxtessellatemultitexture", {Float3, Bytes4, Float4, Float4, Float4, Float2, Float4, Float4, Float4}},
        {"gxffxtessellatepointsprite", {Float4, Bytes4}},
        {"gxffxtessellatequadlinecoloronly", {Float3, Bytes4, Float3, Bytes4, Float3}},
        {"gxffxtessellatesoftbumptexture", {Float3, Bytes4, Float4, Unorm16x4, Float2, Float4, Float4, Float4}},
        {"gxffxtessellatesoftmultitexture", {Float3, Bytes4, Float4, Float4, Float4, Float2, Float4, Float4, Float4}},
        {"gxffxtessellatesofttexture", {Float3, Bytes4, Float4, Unorm16x4, Float2, Float4, Float4, Float4}},
        {"gxffxtessellatetexture", {Float3, Bytes4, Float4, Unorm16x4, Float2, Float4, Float4, Float4}},
        {"gxffxtessellatetracerbumptexture", {Float3, Bytes4, Float4, Bytes4, Bytes4, Float4}},
        {"gxffxtessellatetracertexture", {Float3, Bytes4, Float4, Bytes4, Bytes4, Float4}},
        {"gxffxtexture", {Float3, Bytes4, Bytes4, Bytes4, Bytes4, Float2, Float1, Float1, Float2, Float1}},
        {"vs_vfx_grid", {Float4, Float4}},
        {"vs_vfx_optstandardparticle", {Float4, Uint2, Uint2, Uint2, Uint2}},
        {"vs_vfx_raycastquad", {Float4}},
        {"vs_vfx_raydataback", {Float4}},
        {"vs_vfx_raydatafront", {Float4}},
        {"vs_vfx_rayedgedetect", {Float4}},
        {"vs_vfx_sparkparticle", {Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Float4, Ushort4, Ushort4, Ushort4, Ushort4, Float4, Float4, Float4}},
        {"vs_vfx_standardparticle", {Float4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Ushort4, Half4, Half4, Half4, Half4, Half4, Ushort4}},
        {"vs_vfx_wanderingvision", {Float4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Uint4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4, Half4}},
    };
    return layouts;
}

/// A vertex program's fixed layout, when the engine creates it with one and it reads all of it.
const Formats* EngineLayout(const ScannedShader& vertex) {
    std::string_view name{vertex.name};
    if (vertex.bundle == "gxflvershader" || !name.ends_with(".vpo")) {
        return nullptr;
    }
    name.remove_suffix(4);
    const auto& layouts = EngineLayouts();
    const auto it = layouts.find(name);
    return it != layouts.end() && it->second.size() == vertex.attribute_sizes.size() ? &it->second
                                                                                    : nullptr;
}

struct Header {
    u32 version;
    u32 key_size;
};

/// The models' tessellated passes: each local program's pixel programs, by file name. A pass's
/// local program carries the pass's name or its vertex pass's, and "_pn" (the alpha-tested pass
/// uses the plain one); the domain program is checked against the pixel program's inputs.
std::unordered_map<u64, std::set<u64>> ModelTessPixels(const ShaderScan& scan) {
    static constexpr std::array<std::string_view, 3> skins{"", "_skin", "_morphskin"};
    static constexpr std::array<std::string_view, 2> instanced{"", "_ins"};
    static constexpr std::array<std::string_view, 2> clip{"", "@[cl]"};
    std::unordered_map<u64, std::set<u64>> out;
    for (const auto& [file, hash] : scan.model_files) {
        std::string_view name{file};
        if (!name.ends_with(".ppo")) {
            continue;
        }
        name.remove_suffix(4);
        if (name.ends_with("]")) {
            if (const size_t bracket = name.rfind("_["); bracket != name.npos) {
                name = name.substr(0, bracket);
            }
        }
        const size_t split = name.rfind('_');
        if (split == name.npos) {
            continue;
        }
        const auto token = name.substr(split + 1);
        const auto pass = std::find(Passes.begin(), Passes.end(), token);
        if (pass == Passes.end()) {
            continue;
        }
        const std::string stem{name.substr(0, split)};
        std::set<std::string_view> vertex_passes{token, VertexPass(size_t(pass - Passes.begin()))};
        if (token.ends_with('a')) {
            vertex_passes.insert(token.substr(0, token.size() - 1));
        }
        for (const auto vertex : vertex_passes) {
            for (const auto skin : skins) {
                for (const auto ins : instanced) {
                    for (const auto cl : clip) {
                        const auto it = scan.model_files.find(
                            fmt::format("{}_{}{}{}_pn{}.vpo", stem, vertex, skin, ins, cl));
                        if (it != scan.model_files.end()) {
                            out[it->second].insert(hash);
                        }
                    }
                }
            }
        }
    }
    return out;
}

} // namespace

std::vector<TessStages> TessellationSets(const ShaderScan& scan) {
    using File = ShaderScan::File;
    const auto stem = [](const File& f) { return std::string_view{f.name}.substr(0, f.name.size() - 4); };
    const auto plain = [](std::string_view name) {
        std::string out{name};
        for (const std::string_view token : {"@[cl]", "_skin", "_ins"}) {
            for (size_t at; (at = out.find(token)) != out.npos;) {
                out.erase(at, token.size());
            }
        }
        return out;
    };
    std::unordered_map<u64, const ScannedShader*> by_hash;
    for (const auto& p : scan.shaders) {
        by_hash.emplace(p.hash, &p);
    }
    std::vector<const File*> locals, hulls, domains;
    for (const auto& f : scan.tess_files) {
        const auto it = by_hash.find(f.hash);
        if (it == by_hash.end() || f.name.size() < 4) {
            continue;
        }
        (it->second->kind == 6 ? locals : it->second->kind == 7 ? hulls : domains).push_back(&f);
    }
    // A local program feeds the hull of its own name, of its name and a suffix, or of its name
    // without the skinned, instanced and clipped variants' marks.
    const auto feeds = [&](const File& l, const File& h) {
        const auto ls = stem(l), hs = stem(h);
        return l.bundle == h.bundle &&
               (ls == hs || (hs.size() > ls.size() && hs.starts_with(ls) && hs[ls.size()] == '_') ||
                plain(ls) == hs || plain(ls) == plain(hs));
    };
    // A domain program follows the hull of its name; an alpha-tested pass ("dpta") the hull of its
    // pass without the mark, when no hull has its own name.
    const auto follows = [&](const File& d, const File& h) {
        if (d.bundle != h.bundle) {
            return false;
        }
        const std::string ds{stem(d)};
        const auto hs = stem(h);
        if (ds == hs) {
            return true;
        }
        if (std::ranges::any_of(hulls, [&](const File* other) {
                return other->bundle == d.bundle && stem(*other) == ds;
            })) {
            return false;
        }
        const size_t mark = ds.find("a_");
        return mark != ds.npos && ds.substr(0, mark) + ds.substr(mark + 1) == hs;
    };
    std::set<TessStages> sets;
    for (const File* h : hulls) {
        for (const File* l : locals) {
            if (!feeds(*l, *h)) {
                continue;
            }
            for (const File* d : domains) {
                if (follows(*d, *h)) {
                    sets.insert({l->hash, h->hash, d->hash});
                }
            }
        }
    }
    return {sets.begin(), sets.end()};
}

void NotePipeline(const std::filesystem::path& path, const AheadPipeline& pipeline) {
    static std::mutex mutex;
    static std::unordered_set<u64> noted;
    static bool loaded = false;
    const auto id_of = [](const AheadPipeline& p) {
        // A tessellated draw's hull and domain programs are part of what it is.
        const u64 hull = p.key.stage_hashes[u32(Shader::SwStage::TessellationControl)];
        const u64 domain = p.key.stage_hashes[u32(Shader::SwStage::TessellationEval)];
        return XXH3_64bits_withSeed(reinterpret_cast<const char*>(&p.key) + FormatsStart,
                                    sizeof(p.key) - FormatsStart,
                                    p.vertex ^ p.pixel << 1 ^ hull << 2 ^ domain << 3);
    };
    std::scoped_lock lock{mutex};
    if (!loaded) {
        loaded = true;
        for (const auto& known : ReadNotedPipelines(path)) {
            noted.insert(id_of(known));
        }
        if (noted.empty()) {
            const Header header{Serialization::ShaderBinaryVersion, u32(sizeof(GraphicsPipelineKey))};
            std::ofstream(path, std::ios::binary | std::ios::trunc)
                .write(reinterpret_cast<const char*>(&header), sizeof(header));
        }
    }
    if (!noted.insert(id_of(pipeline)).second) {
        return;
    }
    std::ofstream file(path, std::ios::binary | std::ios::app);
    file.write(reinterpret_cast<const char*>(&pipeline.vertex), sizeof(pipeline.vertex));
    file.write(reinterpret_cast<const char*>(&pipeline.pixel), sizeof(pipeline.pixel));
    file.write(reinterpret_cast<const char*>(&pipeline.key), sizeof(pipeline.key));
}

std::vector<AheadPipeline> ReadNotedPipelines(const std::filesystem::path& path) {
    std::vector<AheadPipeline> out;
    std::ifstream file(path, std::ios::binary);
    Header header{};
    if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        header.version != Serialization::ShaderBinaryVersion ||
        header.key_size != sizeof(GraphicsPipelineKey)) {
        return out;
    }
    AheadPipeline pipeline;
    while (file.read(reinterpret_cast<char*>(&pipeline.vertex), sizeof(pipeline.vertex)) &&
           file.read(reinterpret_cast<char*>(&pipeline.pixel), sizeof(pipeline.pixel)) &&
           file.read(reinterpret_cast<char*>(&pipeline.key), sizeof(pipeline.key))) {
        out.push_back(pipeline);
    }
    return out;
}

u32 CountStates(std::span<const AheadPipeline> seen) {
    std::unordered_set<std::string> states;
    for (const auto& pipeline : seen) {
        states.insert(StateOf(pipeline.key));
    }
    return u32(states.size());
}

std::vector<AheadPipeline> PlanPipelines(const ShaderScan& scan, std::span<const AheadPipeline> seen) {
    std::vector<AheadPipeline> out;
    std::unordered_map<u64, const ScannedShader*> shaders;
    for (const auto& shader : scan.shaders) {
        shaders.emplace(shader.hash, &shader);
    }
    std::unordered_map<std::string, Counts> fine;
    FormatTable formats;
    for (const auto& pipeline : seen) {
        if (const auto vertex = shaders.find(pipeline.vertex); vertex != shaders.end()) {
            formats.Learn(*vertex->second, pipeline.key);
        }
        if (const auto pixel = shaders.find(pipeline.pixel); pixel != shaders.end()) {
            ++fine[FineKind(*pixel->second)][StateOf(pipeline.key)];
        }
    }
    // A program can serve several passes under different file names.
    std::unordered_map<u64, std::set<std::string>> model_states;
    for (const auto& [file, hash] : scan.model_files) {
        std::string_view name{file};
        if (!name.ends_with(".ppo") || !shaders.contains(hash)) {
            continue;
        }
        name.remove_suffix(4);
        if (const size_t clip = name.find('@'); clip != name.npos) {
            name = name.substr(0, clip);
        }
        if (name.ends_with("]")) {
            name = name.substr(0, name.rfind("_["));
        }
        u32 targets = 0;
        for (u32 colour = shaders.at(hash)->colour; colour != 0; colour >>= 4) {
            targets += (colour & 0xf) != 0;
        }
        ModelStates(name.substr(name.rfind('_') + 1), targets, model_states[hash]);
    }
    const size_t engine_programs = EngineProgramsStart(scan);
    const auto effect = [engine_programs](const ScannedShader& shader) {
        return shader.executable && shader.position < engine_programs;
    };
    std::unordered_map<u64, std::set<std::string>> fixed_states;
    std::set<std::pair<u64, u64>> pairs;
    ModelPairs(scan, pairs);
    ExportPairs(scan, engine_programs, pairs);
    for (const auto& [vertex_hash, pixel_hash] : pairs) {
        const auto vertex = shaders.find(vertex_hash);
        const auto pixel = shaders.find(pixel_hash);
        if (vertex == shaders.end() || pixel == shaders.end()) {
            continue;
        }
        std::set<std::string> states;
        if (const auto model = model_states.find(pixel_hash); model != model_states.end()) {
            states = model->second;
        } else {
            auto& fixed = fixed_states[pixel_hash];
            if (fixed.empty()) {
                if (effect(*pixel->second)) {
                    EffectStates(fixed);
                } else if (pixel->second->executable) {
                    EngineStates(fixed);
                } else if (pixel->second->bundle == "gxffxshader") {
                    ParticleStates(fixed);
                } else {
                    FixedStates(*pixel->second, fixed);
                }
            }
            states = fixed;
        }
        if (const auto kind = fine.find(FineKind(*pixel->second)); kind != fine.end()) {
            for (auto& state : Top(kind->second, StatesPerKind)) {
                states.insert(std::move(state));
            }
        }
        const auto* layout = EngineLayout(*vertex->second);
        auto sets = layout ? std::vector<Formats>{*layout} : formats.For(*vertex->second);
        if (effect(*pixel->second) && sets.size() > 1) {
            sets.resize(1); // their vertex shader takes a full-screen quad
        }
        // Rect lists draw generated corners: a vertex shader that fetches attributes is never one
        // (its rect form cannot be made, and play builds none).
        const bool fetches = !vertex->second->attribute_sizes.empty();
        for (const auto& set : sets) {
            for (const auto& state : states) {
                if ((fetches && IsRectList(state)) || IsPatches(state) ||
                    (WritesDepth(*pixel->second) && !HasDepth(state))) {
                    continue; // a pixel shader that writes depth draws only with a depth target
                }
                AheadPipeline pipeline{.vertex = vertex_hash, .pixel = pixel_hash};
                auto* bytes = reinterpret_cast<char*>(&pipeline.key);
                std::memcpy(bytes + StateStart, state.data(), state.size());
                std::memcpy(bytes + FormatsStart, set.data(), set.size() * sizeof(u32));
                out.push_back(pipeline);
            }
        }
    }
    // Tessellated draws: each set with the pixel programs that read what its domain program
    // exports (the models' by file name), in their states drawn as patches.
    const auto model_tess = ModelTessPixels(scan);
    for (const auto& set : TessellationSets(scan)) {
        const auto local = shaders.find(set.local), hull = shaders.find(set.hull),
                   domain = shaders.find(set.domain);
        if (local == shaders.end() || hull == shaders.end() || domain == shaders.end()) {
            continue;
        }
        const std::set<u8> exports(domain->second->exports.begin(), domain->second->exports.end());
        const auto reads_exports = [&](const ScannedShader& pixel) {
            return std::ranges::all_of(pixel.pixel_inputs, [&](u8 input) { return exports.contains(input); });
        };
        std::vector<const ScannedShader*> pixels;
        if (domain->second->bundle == "gxflvershader") {
            if (const auto it = model_tess.find(set.local); it != model_tess.end()) {
                for (const u64 pixel : it->second) {
                    if (const auto p = shaders.find(pixel); p != shaders.end() && reads_exports(*p->second)) {
                        pixels.push_back(p->second);
                    }
                }
            }
        } else {
            for (const auto& shader : scan.shaders) {
                if (shader.kind == 2 && shader.bundle == domain->second->bundle &&
                    shader.pixel_inputs.size() == exports.size() && reads_exports(shader)) {
                    pixels.push_back(&shader);
                }
            }
        }
        const auto* layout = EngineLayout(*local->second);
        const auto sets = layout ? std::vector<Formats>{*layout} : formats.For(*local->second);
        for (const ScannedShader* pixel : pixels) {
            std::set<std::string> base;
            if (const auto model = model_states.find(pixel->hash); model != model_states.end()) {
                base = model->second;
            } else {
                ParticleStates(base);
            }
            std::set<std::string> states;
            for (const auto& state : base) {
                states.insert(AsPatches(state, hull->second->input_points));
            }
            if (const auto kind = fine.find(FineKind(*pixel)); kind != fine.end()) {
                for (auto& state : Top(kind->second, StatesPerKind)) {
                    if (IsPatches(state)) {
                        states.insert(std::move(state));
                    }
                }
            }
            for (const auto& formats_set : sets) {
                for (const auto& state : states) {
                    if (WritesDepth(*pixel) && !HasDepth(state)) {
                        continue;
                    }
                    AheadPipeline pipeline{.vertex = set.local, .pixel = pixel->hash};
                    auto* bytes = reinterpret_cast<char*>(&pipeline.key);
                    std::memcpy(bytes + StateStart, state.data(), state.size());
                    std::memcpy(bytes + FormatsStart, formats_set.data(), formats_set.size() * sizeof(u32));
                    pipeline.key.stage_hashes[u32(Shader::SwStage::TessellationControl)] = set.hull;
                    pipeline.key.stage_hashes[u32(Shader::SwStage::TessellationEval)] = set.domain;
                    out.push_back(pipeline);
                }
            }
        }
    }
    // Depth only, with no pixel shader: the engine's simple vertex programs.
    for (const auto& shader : scan.shaders) {
        if (shader.kind != 1 || shader.domain || shader.bundle == "gxflvershader" ||
            !shader.attribute_sizes.empty()) {
            continue; // rect lists only: see above
        }
        for (const u32 z : {DepthStencil, DepthOnly, DepthNoClamp}) {
            for (const u32 topology : {RectList, RectListNoFill}) {
                AheadPipeline pipeline{.vertex = shader.hash, .pixel = 0};
                pipeline.key.num_samples = pipeline.key.depth_samples = 1;
                auto* bytes = reinterpret_cast<char*>(&pipeline.key);
                std::memset(bytes + offsetof(GraphicsPipelineKey, logic_op), 0xcc, 1);
                std::memcpy(bytes + sizeof(pipeline.key) - 8, &z, 4);
                std::memcpy(bytes + sizeof(pipeline.key) - 4, &topology, 4);
                const auto* layout = EngineLayout(shader);
                for (const auto& set : layout ? std::vector<Formats>{*layout} : formats.For(shader)) {
                    std::memcpy(bytes + FormatsStart, set.data(), set.size() * sizeof(u32));
                    out.push_back(pipeline);
                }
            }
        }
    }
    return out;
}

void CheckPlan(const ShaderScan& scan, std::span<const AheadPipeline> seen) {
    const auto planned = PlanPipelines(scan, {});
    // Slots past the active targets hold stale registers, and the shader mask without a pixel
    // shader too: no backend reads them.
    const auto clean = [](AheadPipeline pipeline) {
        const u32 count = std::min<u32>(pipeline.key.num_color_attachments, AmdGpu::NUM_COLOR_BUFFERS);
        auto* bytes = reinterpret_cast<char*>(&pipeline.key);
        std::memset(bytes + offsetof(GraphicsPipelineKey, color_buffers) + count * 8, 0, (8 - count) * 8);
        if (pipeline.pixel == 0) {
            pipeline.key.cb_shader_mask = {};
        }
        return pipeline;
    };
    const auto whole = [&](const AheadPipeline& raw) {
        const auto pipeline = clean(raw);
        const auto* bytes = reinterpret_cast<const char*>(&pipeline.key);
        return fmt::format("{:x}|{:x}|{:x}|{:x}|", pipeline.vertex, pipeline.pixel,
                           pipeline.key.stage_hashes[u32(Shader::SwStage::TessellationControl)],
                           pipeline.key.stage_hashes[u32(Shader::SwStage::TessellationEval)]) +
               std::string{bytes + FormatsStart, sizeof(pipeline.key) - FormatsStart};
    };
    std::unordered_set<std::string> exact;
    for (const auto& pipeline : planned) {
        exact.insert(whole(pipeline));
    }
    const auto found = std::count_if(seen.begin(), seen.end(), [&](const AheadPipeline& pipeline) {
        return exact.contains(whole(pipeline));
    });
    std::printf("Plan check: %zu planned; of %zu seen, %zu found\n", planned.size(), seen.size(),
                size_t(found));
    // Each one not planned, with what the plan has for its shaders: the rule that misses it.
    std::unordered_map<u64, const ScannedShader*> by_hash;
    for (const auto& shader : scan.shaders) {
        by_hash.emplace(shader.hash, &shader);
    }
    std::set<std::pair<u64, u64>> planned_pairs;
    std::set<std::tuple<u64, u64, std::string>> planned_states;
    for (const auto& raw : planned) {
        const auto pipeline = clean(raw);
        planned_pairs.emplace(pipeline.vertex, pipeline.pixel);
        planned_states.emplace(pipeline.vertex, pipeline.pixel, StateOf(pipeline.key));
    }
    for (const auto& raw : seen) {
        if (exact.contains(whole(raw))) {
            continue;
        }
        const auto pipeline = clean(raw);
        const auto& key = pipeline.key;
        const auto name = [&](u64 hash) {
            const auto it = by_hash.find(hash);
            if (it == by_hash.end()) {
                return fmt::format("? {:x}", hash);
            }
            const auto& shader = *it->second;
            return shader.executable ? fmt::format("exe@{:x} {:x}", shader.position, hash)
                                     : shader.bundle + "/" + shader.name;
        };
        std::string targets;
        for (u32 i = 0; i < key.num_color_attachments && i < AmdGpu::NUM_COLOR_BUFFERS; ++i) {
            u32 blend = 0, mask = 0;
            std::memcpy(&blend, reinterpret_cast<const char*>(&key) + offsetof(GraphicsPipelineKey, blend_controls) + i * 4, 4);
            std::memcpy(&mask, reinterpret_cast<const char*>(&key) + offsetof(GraphicsPipelineKey, write_masks) + i * 4, 4);
            u64 colour = 0;
            std::memcpy(&colour, &key.color_buffers[i], 8);
            targets += fmt::format(" [{:x} {:x}/{:x}]", colour, blend, mask);
        }
        u32 topology = 0, z = 0;
        std::memcpy(&z, reinterpret_cast<const char*>(&key) + sizeof(key) - 8, 4);
        std::memcpy(&topology, reinterpret_cast<const char*>(&key) + sizeof(key) - 4, 4);
        const char* why = !planned_pairs.contains({pipeline.vertex, pipeline.pixel}) ? "pair not planned"
                          : planned_states.contains({pipeline.vertex, pipeline.pixel, StateOf(key)})
                              ? "state planned, vertex formats differ"
                              : "pair planned, state not";
        std::string formats;
        for (const auto format : key.vertex_buffer_formats) {
            if (u32(format) == 0) {
                break;
            }
            formats += fmt::format(" {}", u32(format));
        }
        std::printf("Plan check: missed %s + %s: %s; z %x topology %x%s; vertex formats%s\n",
                    name(pipeline.vertex).c_str(), name(pipeline.pixel).c_str(), why, z, topology,
                    targets.c_str(), formats.c_str());
        if (!planned_pairs.contains({pipeline.vertex, pipeline.pixel})) {
            const auto list = [](std::span<const u8> values) {
                std::string text;
                for (const u8 value : values) {
                    text += fmt::format(" {}", value);
                }
                return text;
            };
            const auto vertex = by_hash.find(pipeline.vertex);
            const auto pixel = by_hash.find(pipeline.pixel);
            if (vertex != by_hash.end() && pixel != by_hash.end()) {
                std::printf("Plan check:   vertex exports%s; pixel inputs%s\n",
                            list(vertex->second->exports).c_str(), list(pixel->second->pixel_inputs).c_str());
            }
        }
        // The planned state of the pair nearest to it, by the key bytes that differ.
        const auto state = StateOf(key);
        std::string nearest;
        size_t fewest = state.size() + 1;
        for (const auto& [vertex, pixel, planned_state] : planned_states) {
            if (vertex != pipeline.vertex || pixel != pipeline.pixel) {
                continue;
            }
            std::string differ;
            size_t count = 0;
            for (size_t i = 0; i < state.size(); ++i) {
                if (state[i] != planned_state[i]) {
                    ++count;
                    differ += fmt::format(" +{:x}: {:02x} planned {:02x}", StateStart + i, u8(state[i]),
                                          u8(planned_state[i]));
                }
            }
            if (count < fewest) {
                fewest = count;
                nearest = differ;
            }
        }
        if (!nearest.empty()) {
            std::printf("Plan check:   nearest planned state differs at%s\n", nearest.c_str());
        }
    }
    // Every engine vertex program: what the plan knows of its attributes and gives it.
    const FormatTable table;
    for (const auto& shader : scan.shaders) {
        if (shader.kind != 1 || shader.domain || shader.bundle == "gxflvershader") {
            continue;
        }
        std::string sizes, sets;
        for (size_t i = 0; i < shader.attribute_sizes.size(); ++i) {
            sizes += fmt::format(" {}{}", shader.attribute_sizes[i],
                                 i < shader.semantics.size() ? ":" + shader.semantics[i] : "");
        }
        const auto* layout = EngineLayout(shader);
        for (const auto& set : layout ? std::vector<Formats>{*layout} : table.For(shader)) {
            sets += " [";
            for (const u32 format : set) {
                sets += fmt::format(" {}", format);
            }
            sets += " ]";
        }
        const auto line = fmt::format(
            "Plan check: vertex {} {:x}; attributes{}; {}{}\n",
            shader.executable ? fmt::format("exe@{:x}", shader.position) : shader.bundle + "/" + shader.name,
            shader.hash, sizes, layout ? "engine layout" : "planned", sets);
        std::fputs(line.c_str(), stdout);
    }
    std::fflush(stdout);
}

} // namespace Vulkan
