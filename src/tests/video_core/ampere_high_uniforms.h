// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <span>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

// Uniform registers UR28-UR62 used to be unmapped, and a program naming one was rejected. They are
// mirrored just below R216 now: the lowering first reports how many mirror registers the program
// needs (UR28 is the first, in pairs), then lowers it again with UR28 at R(216 - needed). Checks
// both steps for a program whose highest uniform register is `uniform`, and that the lowered code
// names its mirror (unless the instruction's lowering never reads that operand).
inline void RequireHighUniformMirror(std::span<const Shader::Ampere::Instruction> code, u32 uniform,
                                     bool named = true) {
    const u32 needed = (uniform - 26) & ~1U;
    const auto without = Shader::Ampere::LowerToMaxwellWith(code, 0, false, false);
    REQUIRE_FALSE(static_cast<bool>(without));
    REQUIRE(without.high_uniform_registers == needed);
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    if (!named) return;
    const u32 mirror = 216 - needed + (uniform - 28);
    REQUIRE(std::any_of(lowered.instructions.begin(), lowered.instructions.end(), [&](u64 word) {
        return (word & 0xff) == mirror || ((word >> 8) & 0xff) == mirror ||
               ((word >> 20) & 0xff) == mirror || ((word >> 39) & 0xff) == mirror;
    }));
}
