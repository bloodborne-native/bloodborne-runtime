// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 micro_tiled_mips;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
};

TileManager::TileManager(Backend::Device& device_, VideoCore::Backend::Transfers& runtime_,
                         StreamBuffer& stream_buffer_)
    : device{device_}, runtime{runtime_}, stream_buffer{stream_buffer_} {
    // bbport: the tiling kernels the game uses, made at start rather than at their first image
    // (on Metal each tile mode and width is a kernel of its own, compiled when it is made): the
    // modes and pixel widths a session in Iosefka's Clinic tiles and detiles.
    using AmdGpu::TileMode;
    static constexpr std::array<std::pair<TileMode, u32>, 9> used{{
        {TileMode::Thin1DThin, 8}, {TileMode::Thin1DThin, 32}, {TileMode::Thin1DThin, 64},
        {TileMode::Thin1DThin, 128}, {TileMode::Thin2DThin, 32}, {TileMode::Thin2DThin, 64},
        {TileMode::Display2DThin, 32}, {TileMode::Depth2DThin64, 16},
        {TileMode::Depth2DThin64, 32},
    }};
    for (const auto& [mode, bits] : used) {
        ImageInfo info{};
        info.tile_mode = mode;
        info.array_mode = AmdGpu::GetArrayMode(mode);
        info.num_bits = bits;
        info.num_samples = 1;
        GetTilingPipeline(info, false);
        GetTilingPipeline(info, true);
    }
}

TileManager::~TileManager() = default;

const Backend::OwnedPipeline& TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .is_tiler = is_tiler,
    };
    if (const auto it = tiling_pipelines.find(key); it != tiling_pipelines.end()) {
        return it->second;
    }

    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool is_macro = AmdGpu::IsMacroTiled(info.array_mode);
    std::array<u32, 12> spec_data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec_data[4] = u32(info.array_mode);
        spec_data[5] = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        spec_data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        spec_data[7] = AmdGpu::GetBankHeight(macro_tile_mode);
        spec_data[8] = AmdGpu::GetNumBanks(macro_tile_mode);
        spec_data[9] = std::bit_width(spec_data[8]) - 1;
        spec_data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                                   info.num_bits);
        spec_data[11] = AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }

    std::span<const u32> code;
    switch (info.num_bits) {
    case 8:
        code = is_macro ? std::span<const u32>{TILING_MACRO_8_COMP}
                        : std::span<const u32>{TILING_MICRO_8_COMP};
        break;
    case 16:
        code = is_macro ? std::span<const u32>{TILING_MACRO_16_COMP}
                        : std::span<const u32>{TILING_MICRO_16_COMP};
        break;
    case 32:
        code = is_macro ? std::span<const u32>{TILING_MACRO_32_COMP}
                        : std::span<const u32>{TILING_MICRO_32_COMP};
        break;
    case 64:
        code = is_macro ? std::span<const u32>{TILING_MACRO_64_COMP}
                        : std::span<const u32>{TILING_MICRO_64_COMP};
        break;
    case 96:
        code = is_macro ? std::span<const u32>{TILING_MACRO_96_COMP}
                        : std::span<const u32>{TILING_MICRO_96_COMP};
        break;
    case 128:
        code = is_macro ? std::span<const u32>{TILING_MACRO_128_COMP}
                        : std::span<const u32>{TILING_MICRO_128_COMP};
        break;
    default:
        UNREACHABLE_MSG("Unsupported tiling pixel width {}", info.num_bits);
    }

    const u32 spec_count = is_macro ? 12 : 4;
    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Creating tiling pipeline {}", module_name);
    namespace Backend = VideoCore::Backend;
    constexpr std::array slots{
        Backend::BindingSlot{.kind = Backend::BindingKind::StorageBuffer,
                             .stages = Backend::ShaderStage::Compute},
        Backend::BindingSlot{.kind = Backend::BindingKind::StorageBuffer,
                             .stages = Backend::ShaderStage::Compute},
        Backend::BindingSlot{.kind = Backend::BindingKind::UniformBuffer,
                             .stages = Backend::ShaderStage::Compute},
    };
    const auto [it, _] = tiling_pipelines.emplace(
        key, Backend::MakeCompute(device, code, slots, 0, module_name,
                                  std::span{spec_data.data(), spec_count}));
    return it->second;
}

void TileManager::RecordTilingDispatch(const Backend::OwnedPipeline& pipeline,
                                       const Backend::BufferBinding& tiled,
                                       const Backend::BufferBinding& linear,
                                       const Backend::BufferBinding& params, u32 dim_x) {
    const std::array buffers{tiled, linear, params};
    auto& recorder = device.Recorder();
    const std::array writes{
        Backend::SlotWrite{.slot = 0, .count = 1, .kind = Backend::BindingKind::StorageBuffer,
                           .buffers = &buffers[0]},
        Backend::SlotWrite{.slot = 1, .count = 1, .kind = Backend::BindingKind::StorageBuffer,
                           .buffers = &buffers[1]},
        Backend::SlotWrite{.slot = 2, .count = 1, .kind = Backend::BindingKind::UniformBuffer,
                           .buffers = &buffers[2]},
    };
    recorder.BindResources(pipeline.Allocation(), writes, {}, {}, buffers);
    recorder.Dispatch(
        {.pipeline = pipeline.Allocation().native, .groups = {dim_x, 1, 1}},
        {.kind = Backend::CrumbKind::Dispatch, .name = "tiling", .count = {dim_x, 1, 1}});
}

std::pair<const Buffer*, u64> TileManager::DetileImage(const VideoCore::Buffer* in_buffer,
                                                       u64 in_offset, const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_tiled_mips = info.micro_tiled_mips;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const auto params_buffer_info = BindingOf(
        stream_buffer,
        stream_buffer.Copy(&params, sizeof(params),
                           device.GetCapabilities().uniform_min_alignment),
        sizeof(params));

    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);

    device.Recorder().EndPass();
    runtime.FlushBarriers();

    const auto tiled_buffer_info = BindingOf(*in_buffer, in_offset, info.guest_size);
    const auto linear_buffer_info = BindingOf(*staging.buffer, staging.offset, info.guest_size);
    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    // bbport: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(GetTilingPipeline(info, false), tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, dim_x);
    // bbport: the source may be the arena (BB_PREUPLOAD), which later uploads write: a copy into
    // it must wait for this read.
    runtime.AccessBuffer(in_buffer, in_offset, info.guest_size,
                         VideoCore::Backend::BufferAccess::ComputeRead);

    runtime.AccessBuffer(staging.buffer, staging.offset, info.guest_size,
                         VideoCore::Backend::BufferAccess::ComputeReadWrite);

    return {staging.buffer, staging.offset};
}

void TileManager::TileImage(Image& in_image, std::span<Backend::BufferImageCopy> buffer_copies,
                            const VideoCore::Buffer* out_buffer, u64 out_offset) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.buffer_offset += out_offset;
        }
        runtime.DownloadImage(&in_image, out_buffer, buffer_copies);
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.micro_tiled_mips = info.micro_tiled_mips;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const auto params_buffer_info = BindingOf(
        stream_buffer,
        stream_buffer.Copy(&params, sizeof(params),
                           device.GetCapabilities().uniform_min_alignment),
        sizeof(params));

    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);
    for (auto& copy : buffer_copies) {
        copy.buffer_offset += staging.offset;
    }

    runtime.DownloadImage(&in_image, staging.buffer, buffer_copies);
    runtime.FlushBarriers();

    const auto tiled_buffer_info = BindingOf(*out_buffer, out_offset, info.guest_size);
    const auto linear_buffer_info = BindingOf(*staging.buffer, staging.offset, info.guest_size);
    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    // bbport: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(GetTilingPipeline(info, true), tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, dim_x);

    runtime.AccessBuffer(out_buffer, out_offset, info.guest_size,
                         VideoCore::Backend::BufferAccess::ComputeReadWrite);
}

} // namespace VideoCore
