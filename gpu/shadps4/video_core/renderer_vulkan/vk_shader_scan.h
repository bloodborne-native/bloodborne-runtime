// bbport: finds the game's shaders in its own files on the player's device, for the ahead-of-time
// compile. Nothing it reads is kept outside the player's cache.
#pragma once

#include <array>
#include <filesystem>
#include <vector>

#include "common/types.h"

namespace Vulkan {

/// One program: what its header gives, in the registers' own form.
struct ScannedShader {
    u64 hash{};
    u32 kind{}; ///< 1 vertex, 2 pixel, 4 compute
    u32 rsrc1{};
    u32 rsrc2{};
    u32 z_format{}; ///< pixel: export format, bit 8 front_face_all_bits; compute: thread count x
    u32 colour{};   ///< pixel: export formats, one nibble per target; compute: thread count y
    u32 enable{};   ///< pixel: SPI_PS_INPUT_ENA; compute: thread count z
    u32 address{};  ///< pixel: SPI_PS_INPUT_ADDR
    u32 inputs{};   ///< pixel: interpolated inputs
    bool executable{};
    size_t offset{}; ///< into ShaderScan::code, in bytes
    size_t length{};
    std::vector<u8> input_order; ///< pixel: each input's parameter
    /// Vertex: the number class of each attribute (0 float, 1 signed, 2 unsigned), the likely sets.
    std::vector<std::vector<u8>> attribute_classes;
};

struct ShaderScan {
    std::vector<ScannedShader> shaders;
    std::vector<u32> code;
};

/// game_dir holds eboot.bin and dvdroot_ps4. Bundled shaders come first, as the engine prefers them.
ShaderScan ScanGameShaders(const std::filesystem::path& game_dir);

} // namespace Vulkan
