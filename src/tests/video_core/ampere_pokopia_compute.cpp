// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "tests/video_core/ampere_high_uniforms.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

bool Contains(const std::vector<u64>& code, u64 word) {
    return std::find(code.begin(), code.end(), word) != code.end();
}

TEST_CASE("Ampere SHF.L.U32.HI immediate without .W lowers to a clamping SHL",
          "[video_core][ampere]") {
    // SHF.L.U32.HI R16, RZ, R14, 0xff and R12, RZ, R11, 0xff from Pokemon Pokopia's compute
    // shaders (op 419 ff107419 000000ff 0001060e 080fe400 and the ff0c7419 form).
    struct Case { Instruction shf; u32 dest, shift; };
    for (const Case c : {Case{{{0xff107419, 0xff, 0x0001060e, 0x080fe400}}, 0x10, 0x0e},
                         Case{{{0xff0c7419, 0xff, 0x0001060b, 0x080fe400}}, 0x0c, 0x0b}}) {
        const auto lowered = LowerToMaxwell(std::array{c.shf, Exit});
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        // SHL Rd, R250 (= 0xff), Rshift without .W (bit 39): a shift of 32 or more gives zero.
        const u64 shl = 0x5c48000000070000ULL | c.dest | (u64{250} << 8) | (u64{c.shift} << 20);
        REQUIRE(Contains(lowered.instructions, shl));
        // .W (bit 75) wraps the shift instead.
        auto wrap = c.shf;
        wrap.words[2] |= 1U << 11;
        const auto wrapped = LowerToMaxwell(std::array{wrap, Exit});
        REQUIRE(static_cast<bool>(wrapped));
        REQUIRE(Contains(wrapped.instructions, shl | (u64{1} << 39)));
        // Other modes stay rejected.
        for (const u32 bit : {0U, 8U, 30U}) {
            auto extended = c.shf;
            extended.words[3] ^= 1U << bit;
            REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{extended, Exit})));
        }
        auto right = c.shf;
        right.words[2] |= 1U << 12; // .R
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{right, Exit})));
    }
}

TEST_CASE("Ampere LEA with a constant base and no carry out lowers to ISCADD",
          "[video_core][ampere]") {
    // LEA R0, R0, c[0x1][0xb80], 0x8 from Pokemon Pokopia's compute shaders
    // (op a11 00007a11 01002e00 078e40ff 000fe200).
    const Instruction lea{{0x00007a11, 0x01002e00, 0x078e40ff, 0x000fe200}};
    const auto lowered = LowerToMaxwell(std::array{lea, Exit});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // ISCADD R0, R0, R251 (the constant), 8, and no carry compare.
    REQUIRE(Contains(lowered.instructions, 0x5c1804000fb70000ULL));
    REQUIRE(std::none_of(lowered.instructions.begin(), lowered.instructions.end(),
                         [](u64 word) { return (word >> 52) == 0x5b6; }));
    // A carry out to P0 still compares the sum against the constant.
    auto carry = lea;
    carry.words[2] &= ~(7U << 17);
    const auto carried = LowerToMaxwell(std::array{carry, Exit});
    REQUIRE(static_cast<bool>(carried));
    REQUIRE(Contains(carried.instructions, 0x5c1804000fb70000ULL));
    REQUIRE(std::any_of(carried.instructions.begin(), carried.instructions.end(),
                        [](u64 word) { return (word >> 52) == 0x5b6; }));
    // A negated constant (bits 62-63) is not lowered.
    auto negated = lea;
    negated.words[1] |= 1U << 31;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{negated, Exit})));
}

TEST_CASE("Ampere DEPBAR.LE on any scoreboard and wait list is a no-op", "[video_core][ampere]") {
    // DEPBAR.LE SB5, 0x1 and DEPBAR.LE SB0, 0x0, {3,2,1} (op 91a 0000791a 0000800e 00000000
    // 000fce00, Pokemon Pokopia's compute shaders).
    for (const u32 word : {0x0000d040U, 0x0000800eU, 0x00008000U, 0x00008fffU, 0x0000ffffU}) {
        const Instruction wait{{0x0000791a, word, 0, 0x000fce00}};
        const auto lowered = LowerToMaxwell(std::array{wait, Exit});
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        REQUIRE(lowered.instructions.size() == 1);
        REQUIRE(lowered.instructions.back() == 0xe30000000007000fULL);
    }
    for (const Instruction invalid : {Instruction{{0x0000791a, 0x0000400e, 0, 0}},
                                      Instruction{{0x0000791a, 0x0001800e, 0, 0}},
                                      Instruction{{0x0000791a, 0x0000800e, 1, 0}},
                                      Instruction{{0x0000191a, 0x0000800e, 0, 0}}}) {
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{invalid, Exit})));
    }
}

TEST_CASE("Ampere UPRMT lowers as PRMT on the GPRs of its uniform registers",
          "[video_core][ampere]") {
    // UPRMT UR6, UR8, 0x9910, URZ (sign-extend the low half) from Pokemon Pokopia's compute
    // shaders (op 896 08067896 00009910 0800003f 000fc800). UR6 and UR8 live in R222 and R224.
    const Instruction uprmt{{0x08067896, 0x00009910, 0x0800003f, 0x000fc800}};
    const auto lowered = LowerToMaxwell(std::array{uprmt, Exit});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const Instruction prmt{{0xe0de7816, 0x00009910, 0x000000ff, 0x000fc800}};
    const auto reference = LowerToMaxwell(std::array{prmt, Exit});
    INFO(reference.error);
    REQUIRE(static_cast<bool>(reference));
    REQUIRE(lowered.instructions == reference.instructions);
    // A uniform third source above UR27 (UR48), once unmapped and rejected: mirrored below R216.
    auto high = uprmt;
    high.words[2] = 0x08000030;
    // (This selector, 0x9910, never reads the third source: the mirror is not named.)
    RequireHighUniformMirror(std::array{high, Exit}, 48, false);
    // No such register: rejected.
    auto unmapped = uprmt;
    unmapped.words[2] = 0x08000040;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unmapped, Exit})));
}

TEST_CASE("Ampere guarded USHF.R.U32.HI lowers to a guarded SHR", "[video_core][ampere]") {
    // @UP1 USHF.R.U32.HI UR9, URZ, 0x2, UR14 from Pokemon Pokopia (op 899 3f091899 00000002
    // 0801160e 000fe400), and the @UP0 / @!UP0 forms.
    for (const u32 guard : {0U, 1U, 8U}) {
        const Instruction guarded{{0x3f090899U | (guard << 12), 0x2, 0x0801160e, 0x000fe400}};
        const Instruction plain{{0x3f097899, 0x2, 0x0801160e, 0x000fe400}};
        const auto lowered = LowerToMaxwell(std::array{guarded, Exit});
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto reference = LowerToMaxwell(std::array{plain, Exit});
        REQUIRE(static_cast<bool>(reference));
        // The same SHR, predicated on P(6-n) where UPn is swapped in from R239.
        const u64 shift = reference.instructions.front();
        const u64 slot = (guard & 8) | (6 - (guard & 7));
        REQUIRE(Contains(lowered.instructions, (shift & ~(u64{0xf} << 16)) | (slot << 16)));
    }
}

TEST_CASE("Ampere STS with a uniform base lowers to IADD and STS", "[video_core][ampere]") {
    // @!P1 STS [UR9+0x130], R32 from Pokemon Pokopia's compute shader 00e063e86f67bf5f
    // (op 988 ff009988 00013020 08000809 000fe800). UR9 lives in R225.
    const Instruction store{{0xff009988, 0x00013020, 0x08000809, 0x000fe800}};
    const auto lowered = LowerToMaxwell(std::array{store, Exit});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const u64 guard = u64{9} << 16;
    REQUIRE(lowered.instructions.size() == 3);
    // R250 = RZ + R225, then STS.32 [R250 + 0x130], R32, both under !P1.
    REQUIRE(lowered.instructions[0] == (0x5c10000000000000ULL | guard | 250 | (u64{255} << 8) |
                                        (u64{225} << 20)));
    REQUIRE(lowered.instructions[1] == (0xef58000000000000ULL | guard | 32 | (u64{250} << 8) |
                                        (u64{0x130} << 20) | (u64{4} << 48)));
    // Without the uniform register (bit 91) bits 64-71 are not an address: rejected.
    auto plain = store;
    plain.words[2] &= ~0x08000000U;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{plain, Exit})));
    // Stored data in one of the lowering's scratch registers (guest R250) used to reject the
    // program. Guest registers from R216 up now live in a second bank (register_pressure.h): the
    // store reads a borrowed low register loaded from it, never the lowering's own R250.
    auto scratch = store;
    scratch.words[1] = (scratch.words[1] & ~0xffU) | 250;
    const auto shadowed = LowerToMaxwell(std::array{scratch, Exit});
    INFO(shadowed.error);
    REQUIRE(static_cast<bool>(shadowed));
    bool stored = false;
    for (const u64 word : shadowed.instructions) {
        if ((word >> 51) != (0xef58000000000000ULL >> 51)) continue;
        stored = true;
        const u32 data = static_cast<u32>(word & 0xff);
        REQUIRE(data < 216);
        REQUIRE(std::find(shadowed.instructions.begin(), shadowed.instructions.end(),
                          Shader::Ampere::NativeRegisterCopy(0, data, 1, 250)) !=
                shadowed.instructions.end());
    }
    REQUIRE(stored);
}

TEST_CASE("Ampere IADD3 with an immediate and a third source keeps the immediate's sign",
          "[video_core][ampere]") {
    // IADD3 R43, R35, -0x7e, -R36 from Pokemon Pokopia's compute shader 4f4f5f3b9e7bb44e
    // (op 810 232b8810 ffffff82 07ffe824 000fe400). Bit 63 is the immediate's sign bit; it was
    // read as a negation of b, giving R35 + 0x7e - R36.
    const Instruction add{{0x232b8810, 0xffffff82, 0x07ffe824, 0x000fe400}};
    const auto lowered = LowerToMaxwell(std::array{add, Exit});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    // MOV32I R251, -0x7e; IADD R250, R35, R251; IADD R43, R250, -R36.
    REQUIRE(lowered.instructions.size() == 4);
    REQUIRE(lowered.instructions[0] == 0x010ffffff828f0fbULL);
    REQUIRE(lowered.instructions[1] == 0x5c1000000fb823faULL);
    REQUIRE(lowered.instructions[2] == 0x5c1100000248fa2bULL);
    // A register b (0x210) still takes bit 63 as its negation.
    const Instruction negated{{0x232b7210, 0x80000022, 0x07ffe824, 0x000fe400}};
    const auto register_form = LowerToMaxwell(std::array{negated, Exit});
    INFO(register_form.error);
    REQUIRE(static_cast<bool>(register_form));
    REQUIRE((register_form.instructions[0] >> 48) == 0x5c11);
}

TEST_CASE("Ampere HFMA2 H0_NH1 swaps the halves of b and negates the new low half",
          "[video_core][ampere]") {
    // HFMA2 R12, R12, R17.H0_NH1, R18 from Pokemon Pokopia's pixel shaders
    // (op 231 0c0c7231 00000011 00400012 000fc800). H0_NH1 names the high half first, as the
    // identity H1_H0 does: b becomes (high H0, low -H1), the rotation of a complex multiply.
    const Instruction hfma{{0x0c0c7231, 0x00000011, 0x00400012, 0x000fc800}};
    const auto lowered = LowerToMaxwell(std::array{hfma, Exit});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == 4);
    // SHF.L R251, R17, 16, R17 (swap); LOP32I.XOR R251, R251, 0x8000; HFMA2 R12, R12, R251, R18.
    REQUIRE(lowered.instructions[0] == 0x36f80880010711fbULL);
    REQUIRE(lowered.instructions[1] == 0x044000080007fbfbULL);
    REQUIRE(lowered.instructions[2] == 0x5d0009000fb70c0cULL);
}

TEST_CASE("Ampere unpredicated R2UR writes the broadcast value directly", "[video_core][ampere]") {
    // R2UR UR5, R6 from Pokemon Pokopia's pixel shaders (op 3c2 060573c2 00000000 000e0000
    // 001e2600). UR5 lives in R221. Unpredicated, every executing lane is in the ballot, so the
    // keep-the-old-value SEL was unreachable; it made the old value a second source for the
    // texture handle and global memory tracking.
    const Instruction move{{0x060573c2, 0, 0x000e0000, 0x001e2600}};
    const auto lowered = LowerToMaxwell(std::array{move, Exit});
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(Contains(lowered.instructions, 0x5c9807800fb700ddULL)); // MOV R221, R251
    REQUIRE(std::none_of(lowered.instructions.begin(), lowered.instructions.end(),
                         [](u64 word) { return (word >> 48) == 0x5ca0; }));
    // Under a predicate a lane may be outside the ballot: the old value stays selectable.
    auto guarded = move;
    guarded.words[0] = (guarded.words[0] & ~0xf000U) | (1U << 12);
    const auto selected = LowerToMaxwell(std::array{guarded, Exit});
    INFO(selected.error);
    REQUIRE(static_cast<bool>(selected));
    REQUIRE(Contains(selected.instructions, 0x5ca003000dd7fbddULL)); // SEL R221, R251, R221, P6
}
} // namespace
