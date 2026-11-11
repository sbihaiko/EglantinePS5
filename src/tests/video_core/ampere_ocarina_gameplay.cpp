// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Instructions that rejected Ocarina of Time's gameplay shaders on the PS5 (the world stayed black
// under the HUD), as logged by the console: each is the first rejected instruction of a program.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "tests/video_core/ampere_high_uniforms.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};
// S2UR UR4, SR 33: makes the program one that uses the uniform bank.
constexpr Instruction UniformBank{{0x000479c3, 0, 0x00002100, 0}};

bool Lowers(const Instruction& instruction) {
    return static_cast<bool>(LowerToMaxwell(std::array{UniformBank, instruction, Exit}));
}
} // namespace

TEST_CASE("Ampere Ocarina gameplay forms lower", "[video_core][ampere][ocarina]") {
    // ISETP.EX whose result is combined with an input predicate (bits 74-75, 87-90).
    REQUIRE(Lowers({{0xff007c0c, 0x00000007, 0x08f05100, 0x000fe200}}));
    REQUIRE(Lowers({{0xff007c0c, 0x00000009, 0x08f25120, 0x000fe200}}));
    // FRND.F32 from a constant buffer.
    REQUIRE(Lowers({{0x00497b07, 0x01404a00, 0x00205000, 0x000e2200}}));
    // TXQ with one output in a single register.
    REQUIRE(Lowers({{0x02027b6f, 0x00801200, 0x000004ff, 0x100f4200}}));
    // RED.F32.ADD at a uniform address.
    REQUIRE(Lowers({{0xff00798e, 0x00000009, 0x0c10e704, 0x0021e800}}));
    // S2R of the variable-rate special register (132).
    REQUIRE(Lowers({{0x00027919, 0x00000000, 0x00008400, 0x000ee200}}));
    // ATOMS.POPC.INC at a byte offset.
    REQUIRE(Lowers({{0x0aff7f8c, 0x00000400, 0x0d00003f, 0x0005e200}}));
    // SHF.R.U64 with an immediate shift.
    REQUIRE(Lowers({{0x0e0e7819, 0x00000015, 0x00001211, 0x020fe200}}));
    // F2I of a negated register.
    REQUIRE(Lowers({{0x00037305, 0x80000013, 0x0021f100, 0x000fe200}}));
    // TLD.LZ with a texel offset.
    REQUIRE(Lowers({{0x04047b66, 0x30000c02, 0x049e18ff, 0x000f4200}}));
    // TLD4 of a 2D array, and with a depth comparison.
    REQUIRE(Lowers({{0x082c7b63, 0xb0801004, 0x001e0f2a, 0x00014200}}));
    REQUIRE(Lowers({{0x180c7b63, 0x30800e1c, 0x001e4f0e, 0x000f4200}}));
    // TEX with the handle in a uniform register, outside the proven handle-extract pattern.
    REQUIRE(Lowers({{0x423b3f60, 0xb0000c3c, 0x081e01ff, 0x00014400}}));
    REQUIRE(Lowers({{0x03000f60, 0x30000a10, 0x0c1e03ff, 0x100fc600}}));
    // Second layer, from the programs the console dumped once the forms above lowered.
    // USHF.R.U64 of a uniform pair, and SHF.R.U64 of a register pair by a register.
    REQUIRE(Lowers({{0x060a7899, 0x00000018, 0x08001207, 0x002fe400}}));
    REQUIRE(Lowers({{0x09147219, 0x0000002b, 0x00001218, 0x004fc400}}));
    // USHF of an immediate by a uniform register.
    REQUIRE(Lowers({{0x3f107499, 0xffffffff, 0x0801060a, 0x000fe400}}));
    // TXD with fewer than four channels.
    REQUIRE(Lowers({{0x14187b6c, 0xa0800a00, 0x041e03ff, 0x001f4600}}));
    REQUIRE(Lowers({{0x1c447b6c, 0xa0800a14, 0x001e0711, 0x0001c200}}));
    // REDUX.MIN.S32.
    REQUIRE(Lowers({{0x000573c4, 0x00000000, 0x00010200, 0x001e2400}}));
    // USEL under a UP0 guard.
    REQUIRE(Lowers({{0x04040887, 0x00000020, 0x08800000, 0x000fe200}}));
    // MOV from a uniform register above UR15.
    REQUIRE(Lowers({{0x00207c02, 0x00000012, 0x08000f00, 0x000fe400}}));
    // A guest register among the lowering's scratch registers (R247).
    REQUIRE(Lowers({{0x6af77220, 0x00000003, 0x00410000, 0x000fc600}}));
}

TEST_CASE("Ampere ISETP.EX keeps every guest predicate but its outputs", "[video_core][ampere][ocarina]") {
    // ISETP.NE.U32.AND.EX P0, P1, R0, UR7, P1 combined: the scratch predicate is saved to R253
    // and restored, and is none of the instruction's own predicates.
    const Instruction compare{{0xff007c0c, 0x00000007, 0x08f05100, 0x000fe200}};
    const auto lowered = LowerToMaxwell(std::array{compare, Exit});
    REQUIRE(static_cast<bool>(lowered));
    const u32 destination = compare.Bits(81, 3), destination_b = compare.Bits(84, 3);
    const u32 low = compare.Bits(68, 3), accumulate = compare.Bits(87, 3);
    const u64 save = lowered.instructions.front();
    REQUIRE((save & 0xfff80000000000ffULL) == (0x38e8000000000000ULL | 253));
    const u32 mask = static_cast<u32>((save >> 20) & 0x7f);
    REQUIRE(std::popcount(mask) == 1);
    const u32 temporary = static_cast<u32>(std::countr_zero(mask));
    for (const u32 used : {destination, destination_b, low, accumulate}) REQUIRE(temporary != used);
    // The restore names the same predicate and is the last word before the EXIT.
    const u64 restore = lowered.instructions[lowered.instructions.size() - 2];
    REQUIRE((restore & 0xfff8000000000000ULL) == 0x38f0000000000000ULL);
    REQUIRE(((restore >> 20) & 0x7f) == mask);
    // The always-false and always-true compares stay rejected.
    for (const u32 op : {0U, 7U}) {
        auto invalid = compare;
        invalid.words[2] = (invalid.words[2] & ~(7U << 12)) | (op << 12);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{invalid, Exit})));
    }
}

TEST_CASE("Ampere guest registers among the uniform mirrors are shadowed", "[video_core][ampere][ocarina]") {
    // LDG R228.. from Ocarina of Time (ffe47981 00002004 0c1e8b00): R228 is UR12's mirror.
    const Instruction load{{0xffe47981, 0x00002004, 0x0c1e8b00, 0x000f6200}};
    const auto lowered = LowerToMaxwell(std::array{load, Exit});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // No lowered word but a native register copy names R216-R245 as its destination.
    const auto is_copy = [](u64 word) {
        return (word & 0xfff8000000000000ULL) == 0x5c98000000000000ULL &&
               ((word >> 39) & 15) == 0 && ((word >> 44) & 15) == 10;
    };
    bool copied_back = false;
    for (const u64 word : lowered.instructions) {
        if (is_copy(word)) {
            copied_back |= ((word >> 8) & 3) == 1 && (word & 0xff) == 228;
            continue;
        }
        if ((word >> 52) == 0xe30) continue; // EXIT
        const u32 target = static_cast<u32>(word & 0xff);
        REQUIRE((target < 216 || target >= 246));
    }
    REQUIRE(copied_back);
    // Without the uniform bank R228 is an ordinary register: nothing is shadowed.
    const Instruction plain{{0x00e47802, 0x00000001, 0x00000f00, 0x000fe200}};
    const auto direct = LowerToMaxwell(std::array{plain, Exit});
    REQUIRE(static_cast<bool>(direct));
    REQUIRE(std::none_of(direct.instructions.begin(), direct.instructions.end(), is_copy));
}

TEST_CASE("Ampere Ocarina gameplay compute forms lower", "[video_core][ampere][ocarina]") {
    // The gameplay upscaler (6f9367bd41dc0b58), which left the world black while it was rejected,
    // and the pass after it (9387db798781f081).
    // LEA.HI Rd, Ra, -Rb, RZ, s (bit 63 negates Rb).
    REQUIRE(Lowers({{0x01017211, 0x80000000, 0x078ff0ff, 0x000fca00}}));
    // SURED with an operation other than ADD (bits 87-89).
    REQUIRE(Lowers({{0x0400159f, 0x60008400, 0x0310e000, 0x0001e200}}));
    // MUFU.RCP of a negated uniform register.
    REQUIRE(Lowers({{0x001e7d08, 0x80000008, 0x08001000, 0x002e2600}}));
}

TEST_CASE("Ampere half per-tap gathers prove offsets across an unrelated LEA.HI",
          "[video_core][ampere][ocarina]") {
    // From upstream Eglantine's tests. Ocarina of Time 1080p upscaler, nvdisasm -b SM87:
    // LEA.HI R8, R8, R7, RZ, 0x1f ; between the offset moves and
    // TLD4.SCR.R.F16.RN.PTP R12, R10, R10, R0, 0x0, 0x30, 2D ;
    const Instruction lea{{0x08087211, 0x00000007, 0x078ff8ff, 0x000fc800}};
    const Instruction gather{{0x0a0c7b63, 0x30003000, 0x001eaf0d, 0x000fc200}};
    auto code = std::array{
        Instruction{{0x00007802, 0xff00ff00, 0xf00, 0}},
        Instruction{{0x00017802, 0x01000001, 0xf00, 0}},
        lea, gather, Exit,
    };
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // The LEA.HI writing one of the offset registers is no proof.
    code[2].words[0] = 0x08017211;
    REQUIRE(LowerToMaxwell(code).error == "native per-tap gather nonconstant offsets");
    // The other rounding/precision modes of the gather (bits 79-80) stay rejected.
    for (const u32 rounding : {2U, 3U}) {
        auto invalid = std::array{code[0], code[1], lea, gather, Exit};
        invalid[3].words[2] = (invalid[3].words[2] & ~(3U << 15)) | (rounding << 15);
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(invalid)));
    }
    // Without the offset moves there are no proven constants.
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{gather, Exit})));
}

TEST_CASE("Ampere Ocarina forms left rejected", "[video_core][ampere][ocarina]") {
    // F2I keeps rejecting the absolute modifier, and a texel offset with .F16 results.
    REQUIRE_FALSE(Lowers({{0x00037305, 0x40000013, 0x0021f100, 0x000fe200}}));
    REQUIRE_FALSE(Lowers({{0x04047b66, 0x30000c02, 0x049e98ff, 0x000f4200}}));
}

TEST_CASE("Ampere Ocarina gameplay programs use uniform registers above UR27",
          "[video_core][ampere][ocarina]") {
    // R2UR UR29, R7 (071d73c2) and R2UR UR28, R7: six gameplay graphics programs.
    RequireHighUniformMirror(std::array{Instruction{{0x071d73c2, 0, 0x000e0000, 0x020ee200}}, Exit}, 29);
    RequireHighUniformMirror(std::array{Instruction{{0x071c73c2, 0, 0x000e0000, 0x020ee200}}, Exit}, 28);
    // The same programs take UBREV and UFLO of UR16 and up, and a guarded UISETP.
    REQUIRE(Lowers({{0x000472be, 0x00000014, 0x08000000, 0x000fc800}}));
    REQUIRE(Lowers({{0x001572bd, 0x00000015, 0x080e0400, 0x000fe400}}));
    REQUIRE(Lowers({{0x00147882, 0x00000020, 0x00000000, 0x000fe200}}));
    REQUIRE(Lowers({{0x1500488c, 0x0000001f, 0x0bf04070, 0x000fe200}}));
    REQUIRE(Lowers({{0x04037c07, 0x00000007, 0x08000000, 0x000fe400}}));
}

TEST_CASE("Ampere Ocarina foliage compute forms lower", "[video_core][ampere][ocarina]") {
    // The programs reached once the foliage culling pipelines run.
    // IADD3.X with two carry inputs.
    REQUIRE(Lowers({{0xff077210, 0x00000001, 0x00fe44ff, 0x000fca00}}));
    REQUIRE(Lowers({{0xff057c10, 0x00000007, 0x087e24ff, 0x000fca00}}));
    // ULDC.U8 and .U16 through an index register.
    REQUIRE(Lowers({{0x06087abb, 0x01002080, 0x08000000, 0x000fe400}}));
    REQUIRE(Lowers({{0x11047abb, 0x01400000, 0x08000400, 0x000fe400}}));
    // F2IP.U8.F32 with a second byte source.
    REQUIRE(Lowers({{0x28297243, 0x00000027, 0x000004ff, 0x000fc400}}));
    // UIADD3 with the carry out in UP5.
    REQUIRE(Lowers({{0x0a0a7890, 0x0000000c, 0x0ff5e03f, 0x000fc800}}));
}
