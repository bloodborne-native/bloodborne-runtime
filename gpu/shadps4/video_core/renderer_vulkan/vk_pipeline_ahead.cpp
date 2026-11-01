// bbport: see vk_pipeline_ahead.h.
#include "video_core/renderer_vulkan/vk_pipeline_ahead.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
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
/// the pixel shader reads. Bundles of full-screen and interface passes also pair on a subset.
void ExportPairs(const ShaderScan& scan, std::set<std::pair<u64, u64>>& pairs) {
    using Set = std::set<u8>;
    std::map<std::pair<std::string, Set>, std::vector<u64>> exact;
    std::map<std::string, std::vector<const ScannedShader*>> vertex_by_group;
    for (const auto& shader : scan.shaders) {
        if (shader.kind == 1 && shader.bundle != "gxflvershader") {
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
        if (const auto it = exact.find({group, inputs}); it != exact.end()) {
            for (const u64 vertex : it->second) {
                pairs.insert({vertex, shader.hash});
            }
        }
        if (group == "gxrenderershader" || group == "gxposteffect" || group == "gxshader" ||
            group == "gxgui") {
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
                // The first texture coordinates differ among meshes more than any other input.
                for (size_t i = 0; i < count; ++i) {
                    if (vertex.semantics[i] == "TEXCOORD") {
                        auto other = named;
                        other[i] = pick(by_semantic.at({"TEXCOORD", vertex.attribute_sizes[i]}), 1);
                        add(std::move(other));
                        break;
                    }
                }
                return out;
            }
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

struct Header {
    u32 version;
    u32 key_size;
};

} // namespace

void NotePipeline(const std::filesystem::path& path, const AheadPipeline& pipeline) {
    static std::mutex mutex;
    static std::unordered_set<u64> noted;
    static bool loaded = false;
    const auto id_of = [](const AheadPipeline& p) {
        return XXH3_64bits_withSeed(reinterpret_cast<const char*>(&p.key) + FormatsStart,
                                    sizeof(p.key) - FormatsStart, p.vertex ^ p.pixel << 1);
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
    if (seen.empty()) {
        return out;
    }
    std::unordered_map<u64, const ScannedShader*> shaders;
    for (const auto& shader : scan.shaders) {
        shaders.emplace(shader.hash, &shader);
    }
    std::unordered_map<std::string, Counts> fine, wide;
    FormatTable formats;
    for (const auto& pipeline : seen) {
        if (const auto vertex = shaders.find(pipeline.vertex); vertex != shaders.end()) {
            formats.Learn(*vertex->second, pipeline.key);
        }
        const auto pixel = shaders.find(pipeline.pixel);
        if (pixel != shaders.end()) {
            ++fine[FineKind(*pixel->second)][StateOf(pipeline.key)];
            ++wide[WideKind(*pixel->second)][StateOf(pipeline.key)];
        }
    }
    std::set<std::pair<u64, u64>> pairs;
    ModelPairs(scan, pairs);
    ExportPairs(scan, pairs);
    for (const auto& [vertex_hash, pixel_hash] : pairs) {
        const auto vertex = shaders.find(vertex_hash);
        const auto pixel = shaders.find(pixel_hash);
        if (vertex == shaders.end() || pixel == shaders.end()) {
            continue;
        }
        const auto kind = fine.find(FineKind(*pixel->second));
        const Counts* counts = kind != fine.end() ? &kind->second : nullptr;
        if (!counts) {
            const auto broad = wide.find(WideKind(*pixel->second));
            if (broad == wide.end()) {
                continue;
            }
            counts = &broad->second;
        }
        const auto states = Top(*counts, StatesPerKind);
        for (const auto& set : formats.For(*vertex->second)) {
            for (const auto& state : states) {
                AheadPipeline pipeline{.vertex = vertex_hash, .pixel = pixel_hash};
                auto* bytes = reinterpret_cast<char*>(&pipeline.key);
                std::memcpy(bytes + StateStart, state.data(), state.size());
                std::memcpy(bytes + FormatsStart, set.data(), set.size() * sizeof(u32));
                out.push_back(pipeline);
            }
        }
    }
    return out;
}

} // namespace Vulkan
