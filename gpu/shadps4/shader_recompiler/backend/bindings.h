// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader::Backend {

struct Bindings {
    u32 unified{};
    u32 buffer{};
    u32 user_data{};

    /// bbport: a stage's bindings from a base of its own, so that its shader does not depend on
    /// how many resources the stages before it use.
    void EnterStage(u32 sw_stage) {
        unified = sw_stage * 256;
        buffer = 0;
    }

    auto operator<=>(const Bindings&) const = default;
};

} // namespace Shader::Backend
