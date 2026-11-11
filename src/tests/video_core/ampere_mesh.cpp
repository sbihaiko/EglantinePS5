// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0x001fea00}};
}

TEST_CASE("Ampere task payload accesses retain byte offsets and distinct input/output regions",
          "[video_core][ampere][mesh]") {
    const Instruction read{{0xff047923, 0, 0x00007800, 0x000e2800}};
    const Instruction write{{0x05007927, 0x00001004, 0x0000b800, 0x0001e200}};
    const auto lowered = LowerToMaxwell(std::array{read, write, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 3);
    const auto input = lowered.instructions[0];
    REQUIRE((input & 0xffff) == 0xff04); // R4 <- [RZ]
    REQUIRE(((input >> 20) & 0xffffff) == 0);
    REQUIRE(((input >> 44) & 31) == 7); // 32-bit attribute input, skew
    const auto output = lowered.instructions[1];
    REQUIRE((output & 0xffff) == 0x0504); // [R5 + 16] <- R4
    REQUIRE(((output >> 20) & 0xffffff) == 16);
    REQUIRE(((output >> 44) & 31) == 27); // 32-bit attribute output, write
}

TEST_CASE("Ampere mesh map writes preserve byte and word widths",
          "[video_core][ampere][mesh]") {
    for (const u32 width : {0U, 0x800U}) {
        const Instruction write{{0x08007927, 0x00000306, 0x8000U | width, 0}};
        const auto lowered = LowerToMaxwell(std::array{write, Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        const auto word = lowered.instructions[0];
        REQUIRE((word & 0xffff) == 0x0806);
        REQUIRE(((word >> 20) & 0xffffff) == 3);
        REQUIRE(((word >> 44) & 1) == (width ? 1 : 0));
        REQUIRE(((word >> 45) & 1) == 0); // map region
        REQUIRE(((word >> 47) & 3) == 3); // output/write
    }
}

TEST_CASE("Ampere stage buffer lowering rejects unrepresented region and direction modes",
          "[video_core][ampere][mesh]") {
    for (const u32 mode : {0x1800U, 0x2800U, 0x7801U, 0xb800U}) {
        INFO(mode);
        const Instruction read{{0xff047923, 0, mode, 0}};
        REQUIRE_FALSE(LowerToMaxwell(std::array{read, Exit}));
    }
    const Instruction input_write{{0x05007927, 4, 0x3800, 0}};
    REQUIRE_FALSE(LowerToMaxwell(std::array{input_write, Exit}));
}

TEST_CASE("Ampere uniform population count uses uniform registers and rejects unsupported modes",
          "[video_core][ampere][mesh]") {
    const Instruction popcount{{0x000272bf, 3, 0x08000000, 0}};
    const auto lowered = LowerToMaxwell(std::array{popcount, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions[0] == (0x5c08000000070000ULL | 218 | (u64{219} << 20)));
    // UR16 and up as the destination or the source were rejected (the handler knew UR0-UR15
    // only); every mirrored uniform register is taken now. UR16 lives in R232.
    const auto high_destination =
        LowerToMaxwell(std::array{Instruction{{0x001072bf, 3, 0x08000000, 0}}, Exit});
    REQUIRE(high_destination);
    REQUIRE(high_destination.instructions[0] == (0x5c08000000070000ULL | 232 | (u64{219} << 20)));
    const auto high_source =
        LowerToMaxwell(std::array{Instruction{{0x000272bf, 16, 0x08000000, 0}}, Exit});
    REQUIRE(high_source);
    REQUIRE(high_source.instructions[0] == (0x5c08000000070000ULL | 218 | (u64{232} << 20)));
    for (const Instruction invalid : std::array{
             Instruction{{0x000262bf, 3, 0x08000000, 0}},
             Instruction{{0x000272bf, 3, 0, 0}},
             Instruction{{0x000272bf, 0x80000003, 0x08000000, 0}},
             Instruction{{0x003f72bf, 3, 0x08000000, 0}},
             Instruction{{0x000272bf, 64, 0x08000000, 0}}}) {
        REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
    }
}
