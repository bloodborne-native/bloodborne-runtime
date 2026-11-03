// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/number_utils.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

#include <atomic>
#include <magic_enum/magic_enum.hpp>

#define INVALID_NUMBER_FORMAT_COMBO                                                                \
    LOG_ERROR(Render_Vulkan, "Unsupported number type {} for format {}", number_type, format);

namespace Vulkan::LiverpoolToVK {

using DepthBuffer = AmdGpu::DepthBuffer;

Backend::StencilOp StencilOp(AmdGpu::StencilFunc op) {
    switch (op) {
    case AmdGpu::StencilFunc::Keep:
        return Backend::StencilOp::Keep;
    case AmdGpu::StencilFunc::Zero:
        return Backend::StencilOp::Zero;
    case AmdGpu::StencilFunc::ReplaceTest:
        return Backend::StencilOp::Replace;
    case AmdGpu::StencilFunc::AddClamp:
        return Backend::StencilOp::IncrementAndClamp;
    case AmdGpu::StencilFunc::SubClamp:
        return Backend::StencilOp::DecrementAndClamp;
    case AmdGpu::StencilFunc::Invert:
        return Backend::StencilOp::Invert;
    case AmdGpu::StencilFunc::AddWrap:
        return Backend::StencilOp::IncrementAndWrap;
    case AmdGpu::StencilFunc::SubWrap:
        return Backend::StencilOp::DecrementAndWrap;
    case AmdGpu::StencilFunc::ReplaceOp:
        return Backend::StencilOp::Replace;
    case AmdGpu::StencilFunc::Ones:
        LOG_WARNING(Render_Vulkan, "Unsupported stencil op {}, using Replace.",
                    static_cast<u32>(op));
        return Backend::StencilOp::Replace;
    case AmdGpu::StencilFunc::And:
    case AmdGpu::StencilFunc::Or:
    case AmdGpu::StencilFunc::Xor:
    case AmdGpu::StencilFunc::Nand:
    case AmdGpu::StencilFunc::Nor:
    case AmdGpu::StencilFunc::Xnor:
        // Bitwise stencil operations have no Vulkan equivalent; eKeep is the safest fallback.
        LOG_WARNING(Render_Vulkan, "Unsupported bitwise stencil op {}, using Keep.",
                    static_cast<u32>(op));
        return Backend::StencilOp::Keep;
    default:
        UNREACHABLE();
        return Backend::StencilOp::Keep;
    }
}

Backend::CompareOp CompareOp(AmdGpu::CompareFunc func) {
    switch (func) {
    case AmdGpu::CompareFunc::Always:
        return Backend::CompareOp::Always;
    case AmdGpu::CompareFunc::Equal:
        return Backend::CompareOp::Equal;
    case AmdGpu::CompareFunc::GreaterEqual:
        return Backend::CompareOp::GreaterOrEqual;
    case AmdGpu::CompareFunc::Greater:
        return Backend::CompareOp::Greater;
    case AmdGpu::CompareFunc::LessEqual:
        return Backend::CompareOp::LessOrEqual;
    case AmdGpu::CompareFunc::Less:
        return Backend::CompareOp::Less;
    case AmdGpu::CompareFunc::NotEqual:
        return Backend::CompareOp::NotEqual;
    case AmdGpu::CompareFunc::Never:
        return Backend::CompareOp::Never;
    default:
        UNREACHABLE();
        return Backend::CompareOp::Always;
    }
}

bool IsPrimitiveCulled(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::TriangleList:
    case AmdGpu::PrimitiveType::TriangleFan:
    case AmdGpu::PrimitiveType::TriangleStrip:
    case AmdGpu::PrimitiveType::PatchPrimitive:
    case AmdGpu::PrimitiveType::AdjTriangleList:
    case AmdGpu::PrimitiveType::AdjTriangleStrip:
    case AmdGpu::PrimitiveType::QuadList:
    case AmdGpu::PrimitiveType::QuadStrip:
    case AmdGpu::PrimitiveType::Polygon:
        return true;
    case AmdGpu::PrimitiveType::None:
    case AmdGpu::PrimitiveType::PointList:
    case AmdGpu::PrimitiveType::LineList:
    case AmdGpu::PrimitiveType::LineStrip:
    case AmdGpu::PrimitiveType::AdjLineList:
    case AmdGpu::PrimitiveType::AdjLineStrip:
    case AmdGpu::PrimitiveType::RectList: // Screen-aligned rectangles that are not culled
    case AmdGpu::PrimitiveType::LineLoop:
        return false;
    default:
        UNREACHABLE();
        return true;
    }
}

Backend::PrimitiveTopology PrimitiveType(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::PointList:
        return Backend::PrimitiveTopology::PointList;
    case AmdGpu::PrimitiveType::LineList:
        return Backend::PrimitiveTopology::LineList;
    case AmdGpu::PrimitiveType::LineStrip:
        return Backend::PrimitiveTopology::LineStrip;
    case AmdGpu::PrimitiveType::TriangleList:
        return Backend::PrimitiveTopology::TriangleList;
    case AmdGpu::PrimitiveType::TriangleFan:
    case AmdGpu::PrimitiveType::Polygon:
        return Backend::PrimitiveTopology::TriangleFan;
    case AmdGpu::PrimitiveType::TriangleStrip:
        return Backend::PrimitiveTopology::TriangleStrip;
    case AmdGpu::PrimitiveType::QuadStrip:
        LOG_WARNING(Render_Vulkan, "Unimplemented primitive type, using TriangleStrip.");
        return Backend::PrimitiveTopology::TriangleStrip;
    case AmdGpu::PrimitiveType::AdjLineList:
        return Backend::PrimitiveTopology::LineListWithAdjacency;
    case AmdGpu::PrimitiveType::AdjLineStrip:
        return Backend::PrimitiveTopology::LineStripWithAdjacency;
    case AmdGpu::PrimitiveType::AdjTriangleList:
        return Backend::PrimitiveTopology::TriangleListWithAdjacency;
    case AmdGpu::PrimitiveType::AdjTriangleStrip:
        return Backend::PrimitiveTopology::TriangleStripWithAdjacency;
    case AmdGpu::PrimitiveType::PatchPrimitive:
        return Backend::PrimitiveTopology::PatchList;
    case AmdGpu::PrimitiveType::QuadList:
    case AmdGpu::PrimitiveType::RectList:
        return Backend::PrimitiveTopology::PatchList;
    default:
        UNREACHABLE_MSG("Unimplemented primitive type: {}", static_cast<u32>(type));
        return Backend::PrimitiveTopology::TriangleList;
    }
}

Backend::PolygonMode PolygonMode(AmdGpu::PolygonMode mode) {
    switch (mode) {
    case AmdGpu::PolygonMode::Point:
        return Backend::PolygonMode::Point;
    case AmdGpu::PolygonMode::Line:
        return Backend::PolygonMode::Line;
    case AmdGpu::PolygonMode::Fill:
        return Backend::PolygonMode::Fill;
    default:
        UNREACHABLE();
        return Backend::PolygonMode::Fill;
    }
}

Backend::CullMode CullMode(AmdGpu::CullMode mode) {
    switch (mode) {
    case AmdGpu::CullMode::None:
        return Backend::CullMode::None;
    case AmdGpu::CullMode::Front:
        return Backend::CullMode::Front;
    case AmdGpu::CullMode::Back:
        return Backend::CullMode::Back;
    case AmdGpu::CullMode::FrontAndBack:
        return Backend::CullMode::FrontAndBack;
    default:
        UNREACHABLE();
        return Backend::CullMode::None;
    }
}

Backend::FrontFace FrontFace(AmdGpu::FrontFace face) {
    switch (face) {
    case AmdGpu::FrontFace::Clockwise:
        return Backend::FrontFace::Clockwise;
    case AmdGpu::FrontFace::CounterClockwise:
        return Backend::FrontFace::CounterClockwise;
    default:
        UNREACHABLE();
        return Backend::FrontFace::Clockwise;
    }
}

Backend::BlendFactor BlendFactor(AmdGpu::BlendControl::BlendFactor factor) {
    using BlendFactor = AmdGpu::BlendControl::BlendFactor;
    switch (factor) {
    case BlendFactor::Zero:
        return Backend::BlendFactor::Zero;
    case BlendFactor::One:
        return Backend::BlendFactor::One;
    case BlendFactor::SrcColor:
        return Backend::BlendFactor::SrcColor;
    case BlendFactor::OneMinusSrcColor:
        return Backend::BlendFactor::OneMinusSrcColor;
    case BlendFactor::SrcAlpha:
        return Backend::BlendFactor::SrcAlpha;
    case BlendFactor::OneMinusSrcAlpha:
        return Backend::BlendFactor::OneMinusSrcAlpha;
    case BlendFactor::DstAlpha:
        return Backend::BlendFactor::DstAlpha;
    case BlendFactor::OneMinusDstAlpha:
        return Backend::BlendFactor::OneMinusDstAlpha;
    case BlendFactor::DstColor:
        return Backend::BlendFactor::DstColor;
    case BlendFactor::OneMinusDstColor:
        return Backend::BlendFactor::OneMinusDstColor;
    case BlendFactor::SrcAlphaSaturate:
        return Backend::BlendFactor::SrcAlphaSaturate;
    case BlendFactor::ConstantColor:
        return Backend::BlendFactor::ConstantColor;
    case BlendFactor::OneMinusConstantColor:
        return Backend::BlendFactor::OneMinusConstantColor;
    case BlendFactor::Src1Color:
        return Backend::BlendFactor::Src1Color;
    case BlendFactor::InvSrc1Color:
        return Backend::BlendFactor::OneMinusSrc1Color;
    case BlendFactor::Src1Alpha:
        return Backend::BlendFactor::Src1Alpha;
    case BlendFactor::InvSrc1Alpha:
        return Backend::BlendFactor::OneMinusSrc1Alpha;
    case BlendFactor::ConstantAlpha:
        return Backend::BlendFactor::ConstantAlpha;
    case BlendFactor::OneMinusConstantAlpha:
        return Backend::BlendFactor::OneMinusConstantAlpha;
    default:
        UNREACHABLE_MSG("Unknown blend factor: {}", static_cast<u32>(factor));
    }
}

bool IsDualSourceBlendFactor(AmdGpu::BlendControl::BlendFactor factor) {
    using BlendFactor = AmdGpu::BlendControl::BlendFactor;
    switch (factor) {
    case BlendFactor::Src1Color:
    case BlendFactor::Src1Alpha:
    case BlendFactor::InvSrc1Color:
    case BlendFactor::InvSrc1Alpha:
        return true;
    default:
        return false;
    }
}

Backend::BlendOp BlendOp(AmdGpu::BlendControl::BlendFunc func) {
    using BlendFunc = AmdGpu::BlendControl::BlendFunc;
    switch (func) {
    case BlendFunc::Add:
        return Backend::BlendOp::Add;
    case BlendFunc::Subtract:
        return Backend::BlendOp::Subtract;
    case BlendFunc::Min:
        return Backend::BlendOp::Min;
    case BlendFunc::Max:
        return Backend::BlendOp::Max;
    case BlendFunc::ReverseSubtract:
        return Backend::BlendOp::ReverseSubtract;
    default:
        UNREACHABLE_MSG("Unknown blend op: {}", static_cast<u32>(func));
    }
}

Backend::LogicOp LogicOp(AmdGpu::ColorControl::LogicOp logic_op) {
    using LogicOp = AmdGpu::ColorControl::LogicOp;
    switch (logic_op) {
    case LogicOp::Clear:
        return Backend::LogicOp::Clear;
    case LogicOp::Nor:
        return Backend::LogicOp::Nor;
    case LogicOp::AndInverted:
        return Backend::LogicOp::AndInverted;
    case LogicOp::CopyInverted:
        return Backend::LogicOp::CopyInverted;
    case LogicOp::AndReverse:
        return Backend::LogicOp::AndReverse;
    case LogicOp::Invert:
        return Backend::LogicOp::Invert;
    case LogicOp::BrushXor:
        LOG_WARNING(Render_Vulkan, "Unimplemented logic op {:#x}, using closest equivalent",
                    u32(logic_op));
        [[fallthrough]];
    case LogicOp::Xor:
        return Backend::LogicOp::Xor;
    case LogicOp::Nand:
        return Backend::LogicOp::Nand;
    case LogicOp::And:
        return Backend::LogicOp::And;
    case LogicOp::Equiv:
        return Backend::LogicOp::Equivalent;
    case LogicOp::Noop:
        return Backend::LogicOp::NoOp;
    case LogicOp::OrInverted:
        return Backend::LogicOp::OrInverted;
    case LogicOp::Copy:
        return Backend::LogicOp::Copy;
    case LogicOp::OrReverse:
        return Backend::LogicOp::OrReverse;
    case LogicOp::Or:
        return Backend::LogicOp::Or;
    case LogicOp::Set:
        return Backend::LogicOp::Set;
    default:
        UNREACHABLE_MSG("Unknown logic op {}", u32(logic_op));
    }
}

// https://github.com/chaotic-cx/mesa-mirror/blob/0954afff5/src/amd/vulkan/radv_sampler.c#L21
Backend::AddressMode ClampMode(AmdGpu::ClampMode mode) {
    switch (mode) {
    case AmdGpu::ClampMode::Wrap:
        return Backend::AddressMode::Repeat;
    case AmdGpu::ClampMode::Mirror:
        return Backend::AddressMode::MirroredRepeat;
    case AmdGpu::ClampMode::ClampLastTexel:
        return Backend::AddressMode::ClampToEdge;
    case AmdGpu::ClampMode::MirrorOnceHalfBorder:
    case AmdGpu::ClampMode::MirrorOnceBorder:
        LOG_WARNING(Render_Vulkan, "Unimplemented clamp mode {}, using closest equivalent.",
                    static_cast<u32>(mode));
        [[fallthrough]];
    case AmdGpu::ClampMode::MirrorOnceLastTexel:
        return Backend::AddressMode::MirrorClampToEdge;
    case AmdGpu::ClampMode::ClampHalfBorder:
        LOG_WARNING(Render_Vulkan, "Unimplemented clamp mode {}, using closest equivalent.",
                    static_cast<u32>(mode));
        [[fallthrough]];
    case AmdGpu::ClampMode::ClampBorder:
        return Backend::AddressMode::ClampToBorder;
    default:
        UNREACHABLE();
    }
}

Backend::CompareOp DepthCompare(AmdGpu::DepthCompare comp) {
    switch (comp) {
    case AmdGpu::DepthCompare::Never:
        return Backend::CompareOp::Never;
    case AmdGpu::DepthCompare::Less:
        return Backend::CompareOp::Less;
    case AmdGpu::DepthCompare::Equal:
        return Backend::CompareOp::Equal;
    case AmdGpu::DepthCompare::LessEqual:
        return Backend::CompareOp::LessOrEqual;
    case AmdGpu::DepthCompare::Greater:
        return Backend::CompareOp::Greater;
    case AmdGpu::DepthCompare::NotEqual:
        return Backend::CompareOp::NotEqual;
    case AmdGpu::DepthCompare::GreaterEqual:
        return Backend::CompareOp::GreaterOrEqual;
    case AmdGpu::DepthCompare::Always:
        return Backend::CompareOp::Always;
    default:
        UNREACHABLE();
    }
}

Backend::Filter Filter(AmdGpu::Filter filter) {
    switch (filter) {
    case AmdGpu::Filter::Point:
    case AmdGpu::Filter::AnisoPoint:
        return Backend::Filter::Nearest;
    case AmdGpu::Filter::Bilinear:
    case AmdGpu::Filter::AnisoLinear:
        return Backend::Filter::Linear;
    default:
        UNREACHABLE();
    }
}

vk::SamplerReductionMode FilterMode(AmdGpu::FilterMode mode) {
    switch (mode) {
    case AmdGpu::FilterMode::Blend:
        return vk::SamplerReductionMode::eWeightedAverage;
    case AmdGpu::FilterMode::Min:
        return vk::SamplerReductionMode::eMin;
    case AmdGpu::FilterMode::Max:
        return vk::SamplerReductionMode::eMax;
    default:
        UNREACHABLE();
    }
}

Backend::MipmapMode MipFilter(AmdGpu::MipFilter filter) {
    switch (filter) {
    case AmdGpu::MipFilter::Point:
        return Backend::MipmapMode::Nearest;
    case AmdGpu::MipFilter::Linear:
        return Backend::MipmapMode::Linear;
    case AmdGpu::MipFilter::None:
        return Backend::MipmapMode::Nearest;
    default:
        UNREACHABLE();
    }
}

Backend::BorderColor BorderColor(AmdGpu::BorderColor color) {
    switch (color) {
    case AmdGpu::BorderColor::TransparentBlack:
        return Backend::BorderColor::FloatTransparentBlack;
    case AmdGpu::BorderColor::OpaqueBlack:
        return Backend::BorderColor::FloatOpaqueBlack;
    case AmdGpu::BorderColor::White:
        return Backend::BorderColor::FloatOpaqueWhite;
    case AmdGpu::BorderColor::Custom:
        return Backend::BorderColor::FloatCustom;
    default:
        UNREACHABLE();
    }
}

Backend::ComponentSwizzle ComponentSwizzle(AmdGpu::CompSwizzle comp_swizzle) {
    switch (comp_swizzle) {
    case AmdGpu::CompSwizzle::Zero:
        return Backend::ComponentSwizzle::Zero;
    case AmdGpu::CompSwizzle::One:
        return Backend::ComponentSwizzle::One;
    case AmdGpu::CompSwizzle::Red:
        return Backend::ComponentSwizzle::R;
    case AmdGpu::CompSwizzle::Green:
        return Backend::ComponentSwizzle::G;
    case AmdGpu::CompSwizzle::Blue:
        return Backend::ComponentSwizzle::B;
    case AmdGpu::CompSwizzle::Alpha:
        return Backend::ComponentSwizzle::A;
    default:
        UNREACHABLE();
    }
}

Backend::ComponentMapping ComponentMapping(AmdGpu::CompMapping comp_mapping) {
    return Backend::ComponentMapping{
        .r = ComponentSwizzle(comp_mapping.r),
        .g = ComponentSwizzle(comp_mapping.g),
        .b = ComponentSwizzle(comp_mapping.b),
        .a = ComponentSwizzle(comp_mapping.a),
    };
}

// Texel buffer feature flags are not needed as format is interpreted in-shader.
static constexpr vk::FormatFeatureFlags2 BufferRead = vk::FormatFeatureFlagBits2::eVertexBuffer;
static constexpr vk::FormatFeatureFlags2 BufferWrite = static_cast<vk::FormatFeatureFlags2>(0);
static constexpr vk::FormatFeatureFlags2 ImageRead = vk::FormatFeatureFlagBits2::eTransferSrc |
                                                     vk::FormatFeatureFlagBits2::eTransferDst |
                                                     vk::FormatFeatureFlagBits2::eSampledImage;
static constexpr vk::FormatFeatureFlags2 ImageWrite =
    vk::FormatFeatureFlagBits2::eStorageImage |
    vk::FormatFeatureFlagBits2::eStorageReadWithoutFormat |
    vk::FormatFeatureFlagBits2::eStorageWriteWithoutFormat;
static constexpr vk::FormatFeatureFlags2 Mrt = vk::FormatFeatureFlagBits2::eColorAttachment;

// Table 8.13 Data and Image Formats [Sea Islands Series Instruction Set Architecture]
static constexpr vk::FormatFeatureFlags2 GetDataFormatFeatureFlags(
    const AmdGpu::DataFormat data_format) {
    switch (data_format) {
    case AmdGpu::DataFormat::FormatInvalid:
    case AmdGpu::DataFormat::Format8:
    case AmdGpu::DataFormat::Format16:
    case AmdGpu::DataFormat::Format8_8:
    case AmdGpu::DataFormat::Format32:
    case AmdGpu::DataFormat::Format16_16:
    case AmdGpu::DataFormat::Format10_11_11:
    case AmdGpu::DataFormat::Format11_11_10:
    case AmdGpu::DataFormat::Format10_10_10_2:
    case AmdGpu::DataFormat::Format2_10_10_10:
    case AmdGpu::DataFormat::Format8_8_8_8:
    case AmdGpu::DataFormat::Format32_32:
    case AmdGpu::DataFormat::Format16_16_16_16:
    case AmdGpu::DataFormat::Format32_32_32_32:
        return BufferRead | BufferWrite | ImageRead | ImageWrite | Mrt;
    case AmdGpu::DataFormat::Format32_32_32:
        return BufferRead | BufferWrite | ImageRead;
    case AmdGpu::DataFormat::Format5_6_5:
    case AmdGpu::DataFormat::Format1_5_5_5:
    case AmdGpu::DataFormat::Format5_5_5_1:
    case AmdGpu::DataFormat::Format4_4_4_4:
        return ImageRead | ImageWrite | Mrt;
    case AmdGpu::DataFormat::Format8_24:
    case AmdGpu::DataFormat::Format24_8:
    case AmdGpu::DataFormat::FormatX24_8_32:
        return ImageRead | Mrt;
    case AmdGpu::DataFormat::FormatGB_GR:
    case AmdGpu::DataFormat::FormatBG_RG:
    case AmdGpu::DataFormat::Format5_9_9_9:
    case AmdGpu::DataFormat::FormatBc1:
    case AmdGpu::DataFormat::FormatBc2:
    case AmdGpu::DataFormat::FormatBc3:
    case AmdGpu::DataFormat::FormatBc4:
    case AmdGpu::DataFormat::FormatBc5:
    case AmdGpu::DataFormat::FormatBc6:
    case AmdGpu::DataFormat::FormatBc7:
    case AmdGpu::DataFormat::Format4_4:
    case AmdGpu::DataFormat::Format6_5_5:
    case AmdGpu::DataFormat::Format1:
    case AmdGpu::DataFormat::Format1_Reversed:
    case AmdGpu::DataFormat::Format32_As_8:
    case AmdGpu::DataFormat::Format32_As_8_8:
    case AmdGpu::DataFormat::Format32_As_32_32_32_32:
        return ImageRead;
    case AmdGpu::DataFormat::FormatFmask8_S2_F1:
    case AmdGpu::DataFormat::FormatFmask8_S4_F1:
    case AmdGpu::DataFormat::FormatFmask8_S8_F1:
    case AmdGpu::DataFormat::FormatFmask8_S2_F2:
    case AmdGpu::DataFormat::FormatFmask8_S4_F2:
    case AmdGpu::DataFormat::FormatFmask8_S4_F4:
    case AmdGpu::DataFormat::FormatFmask16_S16_F1:
    case AmdGpu::DataFormat::FormatFmask16_S8_F2:
    case AmdGpu::DataFormat::FormatFmask32_S16_F2:
    case AmdGpu::DataFormat::FormatFmask32_S8_F4:
    case AmdGpu::DataFormat::FormatFmask32_S8_F8:
    case AmdGpu::DataFormat::FormatFmask64_S16_F4:
    case AmdGpu::DataFormat::FormatFmask64_S16_F8:
        return ImageRead | ImageWrite;
    }
    UNREACHABLE_MSG("Missing feature flags for data format {}", static_cast<u32>(data_format));
}

// Table 8.13 Data and Image Formats [Sea Islands Series Instruction Set Architecture]
static constexpr vk::FormatFeatureFlags2 GetNumberFormatFeatureFlags(
    const AmdGpu::NumberFormat number_format) {
    switch (number_format) {
    case AmdGpu::NumberFormat::Unorm:
    case AmdGpu::NumberFormat::Snorm:
    case AmdGpu::NumberFormat::Uint:
    case AmdGpu::NumberFormat::Sint:
    case AmdGpu::NumberFormat::Float:
        return BufferRead | BufferWrite | ImageRead | ImageWrite | Mrt;
    case AmdGpu::NumberFormat::Uscaled:
    case AmdGpu::NumberFormat::Sscaled:
    case AmdGpu::NumberFormat::SnormNz:
        return BufferRead | ImageRead;
    case AmdGpu::NumberFormat::Srgb:
        return ImageRead | Mrt;
    case AmdGpu::NumberFormat::Ubnorm:
    case AmdGpu::NumberFormat::UbnormNz:
    case AmdGpu::NumberFormat::Ubint:
    case AmdGpu::NumberFormat::Ubscaled:
        return ImageRead;
    }
    UNREACHABLE_MSG("Missing feature flags for number format {}", static_cast<u32>(number_format));
}

static constexpr SurfaceFormatInfo CreateSurfaceFormatInfo(const AmdGpu::DataFormat data_format,
                                                           const AmdGpu::NumberFormat number_format,
                                                           const Backend::Format vk_format) {
    return {
        .data_format = data_format,
        .number_format = number_format,
        .format = vk_format,
        .flags =
            GetDataFormatFeatureFlags(data_format) & GetNumberFormatFeatureFlags(number_format),
    };
}

std::span<const SurfaceFormatInfo> SurfaceFormats() {
    // Uscaled, Sscaled, and Ubnorm formats are automatically remapped and handled in shader.
    static constexpr std::array formats{
        // Invalid
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Uscaled,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Sscaled,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Uint,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Sint,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::SnormNz,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Float,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Srgb,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Ubnorm,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::UbnormNz,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Ubint,
                                Backend::Format::Undefined),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatInvalid, AmdGpu::NumberFormat::Ubscaled,
                                Backend::Format::Undefined),
        // 8
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::R8Unorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::R8Snorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R8Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R8Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8, AmdGpu::NumberFormat::Srgb,
                                Backend::Format::R8Srgb),
        // 16
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::R16Unorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::R16Snorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R16Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R16Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16, AmdGpu::NumberFormat::Float,
                                Backend::Format::R16Sfloat),
        // 8_8
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::R8G8Unorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::R8G8Snorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R8G8Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R8G8Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8, AmdGpu::NumberFormat::Srgb,
                                Backend::Format::R8G8Srgb),
        // 32
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R32Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R32Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32, AmdGpu::NumberFormat::Float,
                                Backend::Format::R32Sfloat),
        // 16_16
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::R16G16Unorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::R16G16Snorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R16G16Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R16G16Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16, AmdGpu::NumberFormat::Float,
                                Backend::Format::R16G16Sfloat),
        // 10_11_11
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format10_11_11, AmdGpu::NumberFormat::Float,
                                Backend::Format::B10G11R11UfloatPack32),
        // 11_11_10 - Remapped to 10_11_11.
        // 10_10_10_2 - Remapped to 2_10_10_10.
        // 2_10_10_10
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format2_10_10_10, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::A2B10G10R10UnormPack32),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format2_10_10_10, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::A2B10G10R10SnormPack32),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format2_10_10_10, AmdGpu::NumberFormat::Uint,
                                Backend::Format::A2B10G10R10UintPack32),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format2_10_10_10, AmdGpu::NumberFormat::Sint,
                                Backend::Format::A2B10G10R10SintPack32),
        // 8_8_8_8
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::R8G8B8A8Unorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::R8G8B8A8Snorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R8G8B8A8Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R8G8B8A8Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format8_8_8_8, AmdGpu::NumberFormat::Srgb,
                                Backend::Format::R8G8B8A8Srgb),
        // 32_32
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R32G32Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R32G32Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32, AmdGpu::NumberFormat::Float,
                                Backend::Format::R32G32Sfloat),
        // 16_16_16_16
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16_16_16, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::R16G16B16A16Unorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16_16_16, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::R16G16B16A16Snorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16_16_16, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R16G16B16A16Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16_16_16, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R16G16B16A16Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16_16_16,
                                AmdGpu::NumberFormat::SnormNz, Backend::Format::R16G16B16A16Snorm),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format16_16_16_16, AmdGpu::NumberFormat::Float,
                                Backend::Format::R16G16B16A16Sfloat),
        // 32_32_32
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32_32, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R32G32B32Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32_32, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R32G32B32Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32_32, AmdGpu::NumberFormat::Float,
                                Backend::Format::R32G32B32Sfloat),
        // 32_32_32_32
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32_32_32, AmdGpu::NumberFormat::Uint,
                                Backend::Format::R32G32B32A32Uint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32_32_32, AmdGpu::NumberFormat::Sint,
                                Backend::Format::R32G32B32A32Sint),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format32_32_32_32, AmdGpu::NumberFormat::Float,
                                Backend::Format::R32G32B32A32Sfloat),
        // 5_6_5
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format5_6_5, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::R5G6B5UnormPack16),
        // 1_5_5_5
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format1_5_5_5, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::A1R5G5B5UnormPack16),
        // 5_5_5_1
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format5_5_5_1, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::R5G5B5A1UnormPack16),
        // 4_4_4_4
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format4_4_4_4, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::B4G4R4A4UnormPack16),
        // 8_24
        // 24_8
        // X24_8_32
        // GB_GR
        // BG_RG
        // 5_9_9_9
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::Format5_9_9_9, AmdGpu::NumberFormat::Float,
                                Backend::Format::E5B9G9R9UfloatPack32),
        // BC1
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc1, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::Bc1RgbaUnormBlock),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc1, AmdGpu::NumberFormat::Srgb,
                                Backend::Format::Bc1RgbaSrgbBlock),
        // BC2
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc2, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::Bc2UnormBlock),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc2, AmdGpu::NumberFormat::Srgb,
                                Backend::Format::Bc2SrgbBlock),
        // BC3
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc3, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::Bc3UnormBlock),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc3, AmdGpu::NumberFormat::Srgb,
                                Backend::Format::Bc3SrgbBlock),
        // BC4
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc4, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::Bc4UnormBlock),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc4, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::Bc4SnormBlock),
        // BC5
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc5, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::Bc5UnormBlock),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc5, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::Bc5SnormBlock),
        // BC6
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc6, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::Bc6HUfloatBlock),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc6, AmdGpu::NumberFormat::Snorm,
                                Backend::Format::Bc6HSfloatBlock),
        // BC7
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc7, AmdGpu::NumberFormat::Unorm,
                                Backend::Format::Bc7UnormBlock),
        CreateSurfaceFormatInfo(AmdGpu::DataFormat::FormatBc7, AmdGpu::NumberFormat::Srgb,
                                Backend::Format::Bc7SrgbBlock),
    };
    return formats;
}

// Table 8.13 Data and Image Formats [Sea Islands Series Instruction Set Architecture]
static const size_t amd_gpu_data_format_bit_size = 6;   // All values are under 64
static const size_t amd_gpu_number_format_bit_size = 4; // All values are under 16

static size_t GetSurfaceFormatTableIndex(AmdGpu::DataFormat data_format,
                                         AmdGpu::NumberFormat num_format) {
    DEBUG_ASSERT(u32(data_format) < 1 << amd_gpu_data_format_bit_size);
    DEBUG_ASSERT(u32(num_format) < 1 << amd_gpu_number_format_bit_size);
    size_t result = static_cast<size_t>(num_format) |
                    (static_cast<size_t>(data_format) << amd_gpu_number_format_bit_size);
    return result;
}

static auto surface_format_table = []() constexpr {
    std::array<Backend::Format,
               (1 << amd_gpu_data_format_bit_size) * (1 << amd_gpu_number_format_bit_size)>
        result;
    for (auto& entry : result) {
        entry = Backend::Format::Undefined;
    }
    for (const auto& supported_format : SurfaceFormats()) {
        result[GetSurfaceFormatTableIndex(supported_format.data_format,
                                          supported_format.number_format)] =
            supported_format.format;
    }
    return result;
}();

Backend::Format TrySurfaceFormat(AmdGpu::DataFormat data_format, AmdGpu::NumberFormat num_format) {
    return surface_format_table[GetSurfaceFormatTableIndex(data_format, num_format)];
}

Backend::Format SurfaceFormat(AmdGpu::DataFormat data_format, AmdGpu::NumberFormat num_format) {
    Backend::Format result = surface_format_table[GetSurfaceFormatTableIndex(data_format, num_format)];
    if (result != Backend::Format::Undefined || data_format == AmdGpu::DataFormat::FormatInvalid) {
        return result;
    }
    // bbport: a combination no real descriptor has (seen: data_format=6/num_format=1),
    // read from game memory the game may be rewriting (a stale sharp). It stopped the game; now
    // the same data format with a number format that exists keeps the element size, and that
    // draw reads garbage for a frame. Same approach as Supermedo/bloodborne_pc 3e3b760
    // (parsifal295, PR #55) for the number conversion.
    for (const auto fallback : {AmdGpu::NumberFormat::Unorm, AmdGpu::NumberFormat::Float,
                                AmdGpu::NumberFormat::Uint}) {
        result = surface_format_table[GetSurfaceFormatTableIndex(data_format, fallback)];
        if (result != Backend::Format::Undefined) {
            break;
        }
    }
    static std::atomic<u32> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
        LOG_WARNING(Render_Vulkan, "Unknown data_format={} and num_format={} (a stale descriptor?): "
                                   "read as {}", static_cast<u32>(data_format),
                    static_cast<u32>(num_format), Backend::FormatName(result));
    }
    ASSERT_MSG(result != Backend::Format::Undefined, "Unknown data_format={} and num_format={}",
               static_cast<u32>(data_format), static_cast<u32>(num_format));
    return result;
}

static constexpr DepthFormatInfo CreateDepthFormatInfo(
    const DepthBuffer::ZFormat z_format, const DepthBuffer::StencilFormat stencil_format,
    const Backend::Format vk_format) {
    return {
        .z_format = z_format,
        .stencil_format = stencil_format,
        .format = vk_format,
        .flags = vk::FormatFeatureFlagBits2::eDepthStencilAttachment,
    };
}

std::span<const DepthFormatInfo> DepthFormats() {
    using ZFormat = DepthBuffer::ZFormat;
    using StencilFormat = DepthBuffer::StencilFormat;
    static constexpr std::array formats{
        // Invalid
        CreateDepthFormatInfo(ZFormat::Invalid, StencilFormat::Invalid, Backend::Format::Undefined),
        CreateDepthFormatInfo(ZFormat::Invalid, StencilFormat::Stencil8,
                              Backend::Format::D32SfloatS8Uint),
        // 16
        CreateDepthFormatInfo(ZFormat::Z16, StencilFormat::Invalid, Backend::Format::D16Unorm),
        CreateDepthFormatInfo(ZFormat::Z16, StencilFormat::Stencil8, Backend::Format::D16UnormS8Uint),
        // 32_Float
        CreateDepthFormatInfo(ZFormat::Z32Float, StencilFormat::Invalid, Backend::Format::D32Sfloat),
        CreateDepthFormatInfo(ZFormat::Z32Float, StencilFormat::Stencil8,
                              Backend::Format::D32SfloatS8Uint),
    };
    return formats;
}

Backend::Format DepthFormat(DepthBuffer::ZFormat z_format, DepthBuffer::StencilFormat stencil_format) {
    const auto& formats = DepthFormats();
    const auto format =
        std::find_if(formats.begin(), formats.end(), [&](const DepthFormatInfo& format_info) {
            return format_info.z_format == z_format && format_info.stencil_format == stencil_format;
        });
    ASSERT_MSG(format != formats.end(), "Unknown z_format={} and stencil_format={}",
               static_cast<u32>(z_format), static_cast<u32>(stencil_format));
    return format->format;
}

static vk::ClearValue ColorBufferClearValueVk(const AmdGpu::ColorBuffer& color_buffer);

Backend::ClearColor ColorBufferClearValue(const AmdGpu::ColorBuffer& color_buffer) {
    return ColorBufferClearValueVk(color_buffer).color.uint32;
}

static vk::ClearValue ColorBufferClearValueVk(const AmdGpu::ColorBuffer& color_buffer) {
    const auto comp_swizzle = color_buffer.Swizzle();
    const auto format = AmdGpu::DataFormat(color_buffer.info.format);
    const auto number_type = color_buffer.GetFixedNumberFormat();

    const auto& c0 = color_buffer.clear_word0;
    const auto& c1 = color_buffer.clear_word1;

    vk::ClearColorValue color{};

    switch (number_type) {
    case AmdGpu::NumberFormat::Uint:
    case AmdGpu::NumberFormat::Sint:
        color.uint32[3] = 1;
        break;
    default:
        color.float32[3] = 1.0f;
        break;
    }

    switch (format) {
    case AmdGpu::DataFormat::Format8:
        switch (number_type) {
        case AmdGpu::NumberFormat::Unorm:
        case AmdGpu::NumberFormat::Srgb: // Should we handle gamma correction here?
            color.float32[0] = NumberUtils::U8ToUnorm(c0 & 0xff);
            break;
            break;
        case AmdGpu::NumberFormat::Snorm:
        case AmdGpu::NumberFormat::SnormNz:
            color.float32[0] = NumberUtils::S8ToSnorm(c0 & 0xff);
            break;
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0;
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format16:
        switch (number_type) {
        case AmdGpu::NumberFormat::Unorm:
            color.float32[0] = NumberUtils::U16ToUnorm(c0 & 0xffff);
            break;
        case AmdGpu::NumberFormat::Snorm:
        case AmdGpu::NumberFormat::SnormNz:
            color.float32[0] = NumberUtils::S16ToSnorm(c0 & 0xffff);
            break;
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0;
            break;
        case AmdGpu::NumberFormat::Float:
            color.float32[0] = NumberUtils::Uf16ToF32(c0 & 0xffff);
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format8_8:
        switch (number_type) {
        case AmdGpu::NumberFormat::Unorm:
        case AmdGpu::NumberFormat::Srgb: // Should we handle gamma correction here?
            color.float32[0] = NumberUtils::U8ToUnorm(c0 & 0xff);
            color.float32[1] = NumberUtils::U8ToUnorm((c0 >> 8) & 0xff);
            break;
        case AmdGpu::NumberFormat::Snorm:
        case AmdGpu::NumberFormat::SnormNz:
            color.float32[0] = NumberUtils::S8ToSnorm(c0 & 0xff);
            color.float32[1] = NumberUtils::S8ToSnorm((c0 >> 8) & 0xff);
            break;
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0 & 0xff;
            color.uint32[1] = (c0 >> 8) & 0xff;
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format32:
        switch (number_type) {
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0;
            break;
        case AmdGpu::NumberFormat::Float:
            color.float32[0] = *(reinterpret_cast<const float*>(&c0));
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format16_16:
        switch (number_type) {
        case AmdGpu::NumberFormat::Unorm:
            color.float32[0] = NumberUtils::U16ToUnorm(c0 & 0xffff);
            color.float32[1] = NumberUtils::U16ToUnorm((c0 >> 16) & 0xffff);
            break;
        case AmdGpu::NumberFormat::Snorm:
        case AmdGpu::NumberFormat::SnormNz:
            color.float32[0] = NumberUtils::S16ToSnorm(c0 & 0xffff);
            color.float32[1] = NumberUtils::S16ToSnorm((c0 >> 16) & 0xffff);
            break;
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0 & 0xffff;
            color.uint32[1] = (c0 >> 16) & 0xffff;
            break;
        case AmdGpu::NumberFormat::Float:
            color.float32[0] = NumberUtils::Uf16ToF32(c0 & 0xffff);
            color.float32[1] = NumberUtils::Uf16ToF32((c0 >> 16) & 0xffff);
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format10_11_11:
        color.float32[0] = NumberUtils::Uf11ToF32(c0 & 0x7ff);
        color.float32[1] = NumberUtils::Uf11ToF32((c0 >> 11) & 0x7ff);
        color.float32[2] = NumberUtils::Uf10ToF32((c0 >> 22) & 0x3ff);
        break;
    case AmdGpu::DataFormat::Format11_11_10:
        color.float32[0] = NumberUtils::Uf10ToF32(c0 & 0x3ff);
        color.float32[1] = NumberUtils::Uf11ToF32((c0 >> 10) & 0x7ff);
        color.float32[2] = NumberUtils::Uf11ToF32((c0 >> 21) & 0x7ff);
        break;
    case AmdGpu::DataFormat::Format5_9_9_9: {
        int exponent;
        union {
            float f;
            u32 u;
        } scale;

        exponent = (c0 >> 27) - 10;
        scale.u = (exponent + 127) << 23;

        color.float32[0] = (c0 & 0x1ff) * scale.f;
        color.float32[1] = ((c0 >> 9) & 0x1ff) * scale.f;
        color.float32[2] = ((c0 >> 18) & 0x1ff) * scale.f;
        break;
    }
    case AmdGpu::DataFormat::Format10_10_10_2:
        switch (number_type) {
        case AmdGpu::NumberFormat::Unorm:
            color.float32[0] = NumberUtils::U2ToUnorm(c0 & 0x3);
            color.float32[1] = NumberUtils::U10ToUnorm((c0 >> 2) & 0x3ff);
            color.float32[2] = NumberUtils::U10ToUnorm((c0 >> 12) & 0x3ff);
            color.float32[3] = NumberUtils::U10ToUnorm(c0 >> 22);
            break;
        case AmdGpu::NumberFormat::Snorm:
        case AmdGpu::NumberFormat::SnormNz:
            color.float32[0] = NumberUtils::S2ToSnorm(c0 & 0x3);
            color.float32[1] = NumberUtils::S10ToSnorm((c0 >> 2) & 0x3ff);
            color.float32[2] = NumberUtils::S10ToSnorm((c0 >> 12) & 0x3ff);
            color.float32[3] = NumberUtils::S2ToSnorm(c0 >> 22);
            break;
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0 & 0x3;
            color.uint32[1] = (c0 >> 2) & 0x3ff;
            color.uint32[2] = (c0 >> 12) & 0x3ff;
            color.uint32[3] = c0 >> 22;
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format2_10_10_10:
        switch (number_type) {
        case AmdGpu::NumberFormat::Unorm:
            color.float32[0] = NumberUtils::U10ToUnorm(c0 & 0x3ff);
            color.float32[1] = NumberUtils::U10ToUnorm((c0 >> 10) & 0x3ff);
            color.float32[2] = NumberUtils::U10ToUnorm((c0 >> 20) & 0x3ff);
            color.float32[3] = NumberUtils::U2ToUnorm(c0 >> 30);
            break;
        case AmdGpu::NumberFormat::Snorm:
        case AmdGpu::NumberFormat::SnormNz:
            color.float32[0] = NumberUtils::S10ToSnorm(c0 & 0x3ff);
            color.float32[1] = NumberUtils::S10ToSnorm((c0 >> 10) & 0x3ff);
            color.float32[2] = NumberUtils::S10ToSnorm((c0 >> 20) & 0x3ff);
            color.float32[3] = NumberUtils::S2ToSnorm(c0 >> 30);
            break;
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0 & 0x3ff;
            color.uint32[1] = (c0 >> 10) & 0x3ff;
            color.uint32[2] = (c0 >> 20) & 0x3ff;
            color.uint32[3] = c0 >> 30;
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format8_8_8_8:
        switch (number_type) {
        case AmdGpu::NumberFormat::Unorm:
        case AmdGpu::NumberFormat::Srgb: // Should we handle gamma correction here?
            color.float32[0] = NumberUtils::U8ToUnorm(c0 & 0xff);
            color.float32[1] = NumberUtils::U8ToUnorm((c0 >> 8) & 0xff);
            color.float32[2] = NumberUtils::U8ToUnorm((c0 >> 16) & 0xff);
            color.float32[3] = NumberUtils::U8ToUnorm(c0 >> 24);
            break;
        case AmdGpu::NumberFormat::Snorm:
        case AmdGpu::NumberFormat::SnormNz:
            color.float32[0] = NumberUtils::S8ToSnorm(c0 & 0xff);
            color.float32[1] = NumberUtils::S8ToSnorm((c0 >> 8) & 0xff);
            color.float32[2] = NumberUtils::S8ToSnorm((c0 >> 16) & 0xff);
            color.float32[3] = NumberUtils::S8ToSnorm(c0 >> 24);
            break;
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0 & 0xff;
            color.uint32[1] = (c0 >> 8) & 0xff;
            color.uint32[2] = (c0 >> 16) & 0xff;
            color.uint32[3] = c0 >> 24;
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format32_32:
        switch (number_type) {
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0;
            color.uint32[1] = c1;
            break;
        case AmdGpu::NumberFormat::Float:
            color.float32[0] = *(reinterpret_cast<const float*>(&c0));
            color.float32[1] = *(reinterpret_cast<const float*>(&c1));
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format16_16_16_16:
        switch (number_type) {
        case AmdGpu::NumberFormat::Unorm:
            color.float32[0] = NumberUtils::U16ToUnorm(c0 & 0xffff);
            color.float32[1] = NumberUtils::U16ToUnorm((c0 >> 16) & 0xffff);
            color.float32[2] = NumberUtils::U16ToUnorm(c1 & 0xffff);
            color.float32[3] = NumberUtils::U16ToUnorm((c1 >> 16) & 0xffff);
            break;
        case AmdGpu::NumberFormat::Snorm:
        case AmdGpu::NumberFormat::SnormNz:
            color.float32[0] = NumberUtils::S16ToSnorm(c0 & 0xffff);
            color.float32[1] = NumberUtils::S16ToSnorm((c0 >> 16) & 0xffff);
            color.float32[2] = NumberUtils::S16ToSnorm(c1 & 0xffff);
            color.float32[3] = NumberUtils::S16ToSnorm((c1 >> 16) & 0xffff);
            break;
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0 & 0xffff;
            color.uint32[1] = (c0 >> 16) & 0xffff;
            color.uint32[2] = c1 & 0xffff;
            color.uint32[3] = (c1 >> 16) & 0xffff;
            break;
        case AmdGpu::NumberFormat::Float:
            color.float32[0] = NumberUtils::Uf16ToF32(c0 & 0xffff);
            color.float32[1] = NumberUtils::Uf16ToF32((c0 >> 16) & 0xffff);
            color.float32[2] = NumberUtils::Uf16ToF32(c1 & 0xffff);
            color.float32[3] = NumberUtils::Uf16ToF32((c1 >> 16) & 0xffff);
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format32_32_32_32:
        switch (number_type) {
        case AmdGpu::NumberFormat::Uint:
        case AmdGpu::NumberFormat::Sint:
            color.uint32[0] = c0;
            color.uint32[1] = c0;
            color.uint32[2] = c0;
            color.uint32[3] = c1;
            break;
        case AmdGpu::NumberFormat::Float:
            color.float32[0] = *(reinterpret_cast<const float*>(&c0));
            color.float32[1] = *(reinterpret_cast<const float*>(&c0));
            color.float32[2] = *(reinterpret_cast<const float*>(&c0));
            color.float32[3] = *(reinterpret_cast<const float*>(&c1));
            break;
        default:
            INVALID_NUMBER_FORMAT_COMBO;
            break;
        }
        break;
    case AmdGpu::DataFormat::Format5_6_5:
        color.float32[0] = NumberUtils::U5ToUnorm(c0 & 0x1f);
        color.float32[1] = NumberUtils::U6ToUnorm((c0 >> 5) & 0x3f);
        color.float32[2] = NumberUtils::U5ToUnorm(c0 >> 11);
        break;
    case AmdGpu::DataFormat::Format1_5_5_5:
        color.float32[0] = NumberUtils::U5ToUnorm(c0 & 0x1f);
        color.float32[1] = NumberUtils::U5ToUnorm((c0 >> 5) & 0x1f);
        color.float32[2] = NumberUtils::U5ToUnorm((c0 >> 10) & 0x1f);
        color.float32[3] = (c0 >> 15) ? 1.0f : 0.0f;
        break;
    case AmdGpu::DataFormat::Format5_5_5_1:
        color.float32[0] = (c0 & 0x1) ? 1.0f : 0.0f;
        color.float32[1] = NumberUtils::U5ToUnorm((c0 >> 1) & 0x1f);
        color.float32[2] = NumberUtils::U5ToUnorm((c0 >> 6) & 0x1f);
        color.float32[3] = NumberUtils::U5ToUnorm((c0 >> 11) & 0x1f);
        break;
    case AmdGpu::DataFormat::Format4_4_4_4:
        color.float32[0] = NumberUtils::U4ToUnorm(c0 & 0xf);
        color.float32[1] = NumberUtils::U4ToUnorm((c0 >> 4) & 0xf);
        color.float32[2] = NumberUtils::U4ToUnorm((c0 >> 8) & 0xf);
        color.float32[3] = NumberUtils::U4ToUnorm(c0 >> 12);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unsupported color buffer format: {}", format);
        break;
    }

    color.float32 = comp_swizzle.Apply(color.float32);
    return {.color = color};
}

vk::SampleCountFlagBits RawNumSamples(u32 num_samples) {
    switch (num_samples) {
    case 1:
        return vk::SampleCountFlagBits::e1;
    case 2:
        return vk::SampleCountFlagBits::e2;
    case 4:
        return vk::SampleCountFlagBits::e4;
    case 8:
        return vk::SampleCountFlagBits::e8;
    case 16:
        return vk::SampleCountFlagBits::e16;
    default:
        UNREACHABLE();
    }
}

vk::SampleCountFlagBits NumSamples(u32 num_samples, vk::SampleCountFlags supported_flags) {
    vk::SampleCountFlagBits flags = RawNumSamples(num_samples);
    // Half sample counts until supported, with a minimum of 1.
    while (!(supported_flags & flags) && num_samples > 1) {
        num_samples /= 2;
        flags = RawNumSamples(num_samples);
    }
    return flags;
}

} // namespace Vulkan::LiverpoolToVK
