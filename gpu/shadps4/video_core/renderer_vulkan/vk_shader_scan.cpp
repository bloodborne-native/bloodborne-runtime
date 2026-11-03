// bbport: see vk_shader_scan.h. The layouts are the PS4 toolchain's: a "Shdr" header, then the code;
// an "OrbShdr" footer with the length and hash, then a record per input and resource.
#include "video_core/renderer_vulkan/vk_shader_scan.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string_view>
#include <unordered_set>

#include <zlib.h>

namespace Vulkan {
namespace {

using Bytes = std::vector<u8>;

Bytes ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Bytes data(file ? size_t(file.tellg()) : 0);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), data.size());
    return data;
}

template <typename T>
T Read(std::string_view data, size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

u32 ReadBig32(std::string_view data, size_t offset) {
    return __builtin_bswap32(Read<u32>(data, offset));
}

/// A DCX container: a zlib stream after the "DCA" header.
Bytes Decompress(const Bytes& file) {
    const std::string_view view{reinterpret_cast<const char*>(file.data()), file.size()};
    const size_t mark = view.find(std::string_view{"DCA\0", 4});
    if (mark == view.npos) {
        return {};
    }
    const size_t start = mark + ReadBig32(view, mark + 4);
    Bytes out(file.size() * 4);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        return {};
    }
    stream.next_in = const_cast<u8*>(file.data() + start);
    stream.avail_in = uInt(file.size() - start);
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = out.data() + stream.total_out;
        stream.avail_out = uInt(out.size() - stream.total_out);
        result = inflate(&stream, Z_NO_FLUSH);
        if (result == Z_OK && stream.avail_out == 0) {
            out.resize(out.size() * 2);
        }
    }
    out.resize(stream.total_out);
    inflateEnd(&stream);
    return out;
}

std::string_view CString(std::string_view data, size_t offset) {
    const size_t end = data.find('\0', offset);
    return data.substr(offset, end == data.npos ? data.npos : end - offset);
}

bool IsSemantic(std::string_view text) {
    size_t i = 0;
    while (i < text.size() && ((text[i] >= 'A' && text[i] <= 'Z') || text[i] == '_')) {
        ++i;
    }
    if (i == 0) {
        return false;
    }
    return std::all_of(text.begin() + i, text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// The root of Scaleform's vertex input names.
constexpr std::string_view ScaleformInputs = "shaderInput";

bool IsInputName(std::string_view text) {
    if (text.size() < 3 || !std::all_of(text.begin(), text.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '.';
        })) {
        return false;
    }
    const auto root = text.substr(0, text.find('.'));
    static constexpr std::array<std::string_view, 9> roots{
        "vtxInput", "vsIn", "In", "IN", "input", "VertexMain", "vPosition", "vTexCoords",
        ScaleformInputs};
    return std::find(roots.begin(), roots.end(), root) != roots.end();
}

/// The semantic of each vertex input, in attribute order, from the records after the footer:
/// {type, usage, slot, index, 0, name, semantic}, the offsets relative to their own fields. Scaleform's programs (in the executable) give
/// three inputs the same semantic, so theirs is the input's name (`pos`, `acolor`, `afactor`,
/// `atc`, `vbatch`). `limit` bounds the search in a file that holds more than the program.
std::vector<std::string_view> InputSemantics(std::string_view data, size_t footer, size_t limit) {
    std::vector<std::pair<u32, std::string_view>> found;
    const size_t size = std::min(data.size(), footer + limit);
    for (size_t at = footer + 28; at + 16 <= size; at += 4) {
        const u32 zero = Read<u32>(data, at);
        const u32 name = Read<u32>(data, at + 4);
        const u32 semantic = Read<u32>(data, at + 8);
        if (zero == 0 && name > 8 && name < size && semantic > 8 && semantic < size &&
            at + 4 + name < size && at + 8 + semantic < size) {
            const auto name_text = CString(data, at + 4 + name);
            const auto semantic_text = CString(data, at + 8 + semantic);
            if (IsInputName(name_text) && IsSemantic(semantic_text)) {
                const bool scaleform = name_text.starts_with(ScaleformInputs);
                found.emplace_back(u8(data[at - 1]),
                                   scaleform ? name_text.substr(ScaleformInputs.size() + 1) : semantic_text);
            }
        }
    }
    // In index order when the indices are exactly 0..n-1; else as listed.
    std::vector<bool> taken(found.size());
    bool indexed = true;
    for (const auto& [index, semantic] : found) {
        indexed = indexed && index < found.size() && !taken[index];
        if (indexed) {
            taken[index] = true;
        }
    }
    if (indexed) {
        std::stable_sort(found.begin(), found.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    std::vector<std::string_view> out;
    for (const auto& [index, semantic] : found) {
        out.push_back(semantic);
    }
    return out;
}

u8 SemanticClass(std::string_view semantic) {
    if (semantic == "BLENDINDICES" || semantic == "NORMAL" || semantic == "TANGENT" ||
        semantic == "BINORMAL") {
        return 2;
    }
    if (semantic == "vbatch") {
        return 1; // Scaleform's batch index
    }
    return semantic == "TEXCOORD" ? 1 : 0;
}

/// A bundle's file name: UTF-16, cut to the part after the last folder, in lower case.
std::string FileName(std::string_view bundle, size_t offset) {
    std::string name;
    for (size_t at = offset; at + 2 <= bundle.size(); at += 2) {
        const u16 unit = Read<u16>(bundle, at);
        if (unit == 0) {
            break;
        }
        const char c = char(unit);
        name.push_back(c >= 'A' && c <= 'Z' ? char(c + 32) : c);
    }
    const size_t slash = name.rfind('\\');
    return slash == name.npos ? name : name.substr(slash + 1);
}

class Scanner {
public:
    void Add(std::string_view data, size_t header, size_t footer, bool executable,
             std::string_view bundle = {}, std::string name = {}) {
        const u64 hash = Read<u64>(data, footer + 16) & 0xffffffffULL;
        if (const u32 kind = u8(data[header + 8]);
            !bundle.empty() && (kind == 6 || kind == 7 || (kind == 1 && name.ends_with(".dpo")))) {
            out.tess_files.push_back({std::string{bundle}, name, hash});
        }
        if (seen.contains(hash)) {
            return;
        }
        std::array<u32, 60> w{};
        std::memcpy(w.data(), data.data() + header + 8,
                    std::min(sizeof(w), data.size() - header - 8));
        const u32 kind = w[0] & 0xff;
        if (kind != 1 && kind != 2 && kind != 4 && kind != 6 && kind != 7) {
            return;
        }
        seen.insert(hash);
        const size_t code = header + w[4] + 0x10;
        const size_t length = Read<u32>(data, footer + 8) >> 8;

        ScannedShader shader{.hash = hash, .kind = kind, .rsrc1 = w[6], .rsrc2 = w[7],
                             .colour = w[9], .enable = w[10], .executable = executable,
                             .offset = out.code.size() * 4, .length = length};
        shader.position = header;
        shader.bundle = bundle;
        shader.name = std::move(name);
        shader.word13 = w[13];
        shader.depth_control = w[14];
        shader.word15 = w[15];
        out.code.resize(out.code.size() + (length + 3) / 4);
        std::memcpy(reinterpret_cast<char*>(out.code.data()) + shader.offset, data.data() + code,
                    length);
        if (kind == 4) {
            shader.z_format = w[8];
        } else {
            shader.z_format = w[8] | (((w[13] >> 24) & 1) << 8);
            shader.address = w[11];
            shader.inputs = w[12];
        }
        if (kind == 2) {
            const u32 count = w[12];
            const size_t table = code - ((2 * count + 3) & ~3u);
            for (u32 i = 0; i < count; ++i) {
                shader.input_order.push_back(u8(Read<u16>(data, table + i * 2) - 0x0f));
                shader.pixel_inputs.push_back(u8(Read<u16>(data, table + i * 2)));
            }
        }
        if (kind == 1) {
            const u32 exports = (w[11] >> 8) & 0xff;
            const size_t table = code - ((2 * exports + 3) & ~3u);
            for (u32 i = 0; i < exports; ++i) {
                shader.exports.push_back(u8(Read<u16>(data, table + i * 2)));
            }
            shader.domain = shader.name.ends_with(".dpo");
            AddAttributes(shader, data, footer, w, w[11] & 0xff);
        }
        if (kind == 6) {
            // A local program has no export registers: its attribute count comes first, then
            // the output stride the hull program reads its control points with.
            shader.ls_stride = w[9];
            AddAttributes(shader, data, footer, w, w[8] & 0xff);
        }
        if (kind == 7) {
            shader.tess_param = w[8];
            shader.input_points = w[11];
            shader.output_points = w[12];
            shader.patch_constants = w[13];
            shader.output_stride = w[14];
            shader.threads = w[15];
            shader.first_edge = w[17];
        }
        out.shaders.push_back(std::move(shader));
    }

    ShaderScan out;

private:
    void AddAttributes(ScannedShader& shader, std::string_view data, size_t footer,
                       const std::array<u32, 60>& w, u32 count) {
        bool found = false;
        u32 found_at = 0;
        for (u32 k = 12; count != 0 && k < 50 && !found; ++k) {
            found = true;
            for (u32 i = 0; i < count && found; ++i) {
                const u32 entry = k + i < w.size() ? w[k + i] : 0;
                const u32 size = (entry >> 16) & 0xff;
                found = ((entry >> 8) & 0xff) == 4 + 4 * i && size > 0 && size <= 4 &&
                        (entry >> 24) == 0;
            }
            found_at = k;
        }
        if (!found) {
            return;
        }
        for (u32 i = 0; i < count; ++i) {
            shader.attribute_sizes.push_back((w[found_at + i] >> 16) & 0xff);
        }
        std::vector<u8> plain(count), named(count), texcoord_float(count);
        {
            // A program's records follow its footer; in the executable the next program follows.
            const size_t next = shader.executable ? data.find("OrbShdr", footer + 8) : data.npos;
            const auto semantics =
                InputSemantics(data, footer, next == data.npos ? data.size() : next - footer);
            if (semantics.size() == count) {
                for (u32 i = 0; i < count; ++i) {
                    shader.semantics.emplace_back(semantics[i]);
                    named[i] = SemanticClass(semantics[i]);
                    texcoord_float[i] = semantics[i] == "TEXCOORD" ? 0 : named[i];
                }
            }
        }
        shader.attribute_classes.push_back(named);
        if (texcoord_float != named) {
            shader.attribute_classes.push_back(texcoord_float);
        }
        if (plain != named && plain != texcoord_float) {
            shader.attribute_classes.push_back(plain);
        }
    }

    std::unordered_set<u64> seen;
};

} // namespace

ShaderScan ScanGameShaders(const std::filesystem::path& game_dir) {
    Scanner scanner;
    std::vector<std::filesystem::path> bundles;
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(game_dir / "dvdroot_ps4" / "shader", error)) {
        if (entry.path().string().ends_with(".shaderbnd.dcx")) {
            bundles.push_back(entry.path());
        }
    }
    std::sort(bundles.begin(), bundles.end());
    for (const auto& path : bundles) {
        const Bytes bundle = Decompress(ReadFile(path));
        const std::string_view view{reinterpret_cast<const char*>(bundle.data()), bundle.size()};
        if (view.size() < 0x40) {
            continue;
        }
        const std::string name_of_bundle = path.filename().string();
        const std::string stem = name_of_bundle.substr(0, name_of_bundle.find('.'));
        const u32 files = Read<u32>(view, 0xc);
        for (u32 i = 0; i < files; ++i) {
            const size_t record = 0x40 + size_t(i) * 0x24;
            const auto size = Read<u64>(view, record + 8);
            const auto offset = Read<u32>(view, record + 24);
            if (offset + size > view.size()) {
                continue;
            }
            const auto file = view.substr(offset, size);
            const size_t footer = file.find("OrbShdr");
            const size_t header = file.find("Shdr");
            if (footer != file.npos && header < footer) {
                const auto name = FileName(view, Read<u32>(view, record + 32));
                if (stem == "gxflvershader") {
                    scanner.out.model_files.emplace(name, Read<u64>(file, footer + 16) & 0xffffffffULL);
                }
                scanner.Add(file, header, footer, false, stem, name);
            }
        }
    }
    const Bytes executable = ReadFile(game_dir / "eboot.bin");
    const std::string_view view{reinterpret_cast<const char*>(executable.data()), executable.size()};
    std::vector<size_t> headers, footers;
    for (size_t at = view.find("Shdr"); at != view.npos; at = view.find("Shdr", at + 1)) {
        (at >= 3 && view.substr(at - 3, 3) == "Orb" ? footers : headers)
            .push_back(at >= 3 && view.substr(at - 3, 3) == "Orb" ? at - 3 : at);
    }
    for (const size_t header : headers) {
        const auto footer = std::upper_bound(footers.begin(), footers.end(), header);
        if (footer != footers.end()) {
            scanner.Add(view, header, *footer, true);
        }
    }
    return std::move(scanner.out);
}

} // namespace Vulkan
