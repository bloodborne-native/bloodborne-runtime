// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifdef __APPLE__
#include "video_core/renderer_vulkan/vk_common.h"
#include <memory>
#include <string>
#include "video_core/renderer_vulkan/vk_fsr4.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// The Metal FSR 4.1.x modes: FSR 4.1 (exact-fast), FSR 4.1 Lossy (lossy2) and FSR 4.1.2 Lite.
enum class Fsr4MetalMode : int { Fsr411 = 0, Fsr411Lossy = 1, Fsr412 = 2 };

class Fsr4MetalBackend {
public:
    static bool IsSupported(const Instance& instance, Fsr4MetalMode mode);
    static std::unique_ptr<Fsr4MetalBackend> Create(const Instance& instance, Scheduler& scheduler,
                                                   Fsr4MetalMode mode);

    virtual ~Fsr4MetalBackend() = default;
    virtual bool Record(const Fsr4Upscaler::Frame& frame) = 0;
    virtual const char* Problem() const noexcept = 0;
};

} // namespace Vulkan
#endif
