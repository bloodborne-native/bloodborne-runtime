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

class Fsr4MetalBackend {
public:
    static bool IsSupported(const Instance& instance);
    static std::unique_ptr<Fsr4MetalBackend> Create(const Instance& instance, Scheduler& scheduler);

    virtual ~Fsr4MetalBackend() = default;
    virtual bool Record(const Fsr4Upscaler::Frame& frame) = 0;
    virtual const char* Problem() const noexcept = 0;
};

} // namespace Vulkan
#endif
