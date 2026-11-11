// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

// Pokemon Pokopia's fullscreen sky passes leave their loops with @P4 / @P1 CALL.REL.NOINC to the
// instruction after the loop's BRA (op 944 00004944 00000010 03c00000 000fe200 and the 1944
// form). Here they sit beside a library call whose RET.REL.NODEC goes through R2: the transfer
// is a conditional branch, unrelated to that call and its return.
constexpr std::array<Instruction, 8> LoopExitProgram{{
    {{0x00027802, 0x20, 0x00000f00, 0}},              // MOV R2, 0x20 (the call returns to 0x20)
    {{0x00007944, 0x50, 0x03c00000, 0}},              // CALL.REL.NOINC 0x70
    {{0x00004944, 0x10, 0x03c00000, 0x000fe200}},     // @P4 CALL.REL.NOINC 0x40
    {{0x00007947, 0xffffffe0, 0x0383ffff, 0}},        // BRA 0x20
    {{0x00001944, 0x10, 0x03c00000, 0x000fe200}},     // @P1 CALL.REL.NOINC 0x60
    {{0x00007947, 0xffffffe0, 0x0383ffff, 0}},        // BRA 0x40
    {{0x0000794d, 0, 0x03800000, 0}},                 // EXIT
    {{0x02007950, 0xffffff80, 0x03c3ffff, 0}},        // RET.REL.NODEC R2
}};

constexpr u64 BranchFields = ~(u64{0xffffff} << 20);

TEST_CASE("Ampere predicated CALL.REL.NOINC loop exits lower beside a linked return",
          "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(LoopExitProgram);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == sizeof(LoopExitProgram));
    REQUIRE(lowered.instructions.back() == 0xe32000000007000fULL);
    // Each exit is a BRA under its own predicate, not a CAL.
    for (const auto [index, predicate] : {std::pair{2U, 4U}, std::pair{4U, 1U}}) {
        const u64 branch = lowered.instructions[lowered.source_instruction_offsets[index]];
        REQUIRE((branch & BranchFields) == (0xe24000000000000fULL | (u64{predicate} << 16)));
    }
}

TEST_CASE("Ampere predicated CALL.REL.NOINC loop exits accept a conditional loop branch",
          "[video_core][ampere]") {
    auto code = LoopExitProgram;
    code[3].words[0] = 0x00000947; // @P0 BRA
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
}

TEST_CASE("Ampere predicated CALL.REL.NOINC needs the next-but-one target and NOINC",
          "[video_core][ampere]") {
    auto distant = LoopExitProgram;
    distant[2].words[1] = 0x20;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(distant)));
    auto counted = LoopExitProgram;
    counted[2].words[2] &= ~0x00400000U; // without .NOINC
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(counted)));
}
} // namespace
