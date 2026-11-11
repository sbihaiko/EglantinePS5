// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/bit_field.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {

// HMMA.16816.F16 / HMMA.1688.F16 lowered natively (frontend/ampere/lower_to_maxwell.h emits this
// pseudo-instruction): D(16x8) = A(16xK) * B(Kx8) + C with f16 fragments, K = 8 or 16, for GPUs
// without a cooperative-matrix path. Lane t = 4g + q holds rows g and g + 8, columns 2q and 2q + 1
// of C and D; its A registers hold row g/g+8, k = 2q..2q+1 (+8 for the second chunk); its B registers
// hold k = 2q..2q+1 (+8), column g.
//
// Lane t needs A[g][0..K) and A[g+8][0..K), held by lanes 4g..4g+3, and B[0..K)[2q], B[0..K)[2q+1],
// held by lanes 8q..8q+3 and 8q+4..8q+7: one shuffle per source register and lane, 32 for K = 16.
// The Maxwell instruction sequence it replaces shuffled the same values once per product (96).
// Products are accumulated in f32 and rounded to f16 after every 8 k, in the same order as before.
void TranslatorVisitor::HMMA_NATIVE(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> a_reg;
        BitField<20, 8, IR::Reg> b_reg;
        BitField<39, 8, IR::Reg> c_reg;
        BitField<47, 1, u64> deep;
    } const hmma{insn};

    const u32 a = static_cast<u32>(hmma.a_reg.Value());
    const u32 b = static_cast<u32>(hmma.b_reg.Value());
    const u32 c = static_cast<u32>(hmma.c_reg.Value());
    const u32 chunks = hmma.deep != 0 ? 2U : 1U;

    const IR::U32 lane{ir.LaneId()};
    const IR::U32 row_group{ir.BitwiseAnd(lane, ir.Imm32(0x1c))};
    const IR::U32 column_lanes{ir.ShiftLeftLogical(ir.BitwiseAnd(lane, ir.Imm32(3)), ir.Imm32(3))};
    const IR::U32 clamp{ir.Imm32(31)};
    const IR::U32 segment{ir.Imm32(0)};
    const auto unpack = [&](const IR::U32& packed, u32 half) {
        return IR::F32{ir.CompositeExtract(ir.UnpackHalf2x16(packed), half)};
    };
    const auto round_half = [&](const IR::F32& value) {
        return IR::F32{ir.FPConvert(32, ir.FPConvert(16, value))};
    };

    IR::F32 acc[2][2];
    for (u32 row = 0; row < 2; ++row) {
        for (u32 col = 0; col < 2; ++col) {
            acc[row][col] = c == 255 ? ir.Imm32(0.0f) : unpack(X(IR::Reg{c + row}), col);
        }
    }
    for (u32 chunk = 0; chunk < chunks; ++chunk) {
        const IR::U32 a_rows[2]{X(IR::Reg{a + chunk * 2}), X(IR::Reg{a + chunk * 2 + 1})};
        const IR::U32 b_column{X(IR::Reg{b + chunk})};
        for (u32 k = 0; k < 4; ++k) {
            const IR::U32 a_lane{ir.BitwiseOr(row_group, ir.Imm32(k))};
            IR::U32 a_values[2];
            for (u32 row = 0; row < 2; ++row) {
                a_values[row] = ir.ShuffleIndex(a_rows[row], a_lane, clamp, segment);
            }
            IR::U32 b_values[2];
            for (u32 col = 0; col < 2; ++col) {
                const IR::U32 b_lane{ir.IAdd(column_lanes, ir.Imm32(col * 4 + k))};
                b_values[col] = ir.ShuffleIndex(b_column, b_lane, clamp, segment);
            }
            for (u32 row = 0; row < 2; ++row) {
                for (u32 half = 0; half < 2; ++half) {
                    const IR::F32 a_value{unpack(a_values[row], half)};
                    for (u32 col = 0; col < 2; ++col) {
                        acc[row][col] =
                            IR::F32{ir.FPFma(a_value, unpack(b_values[col], half), acc[row][col])};
                    }
                }
            }
        }
        for (u32 row = 0; row < 2; ++row) {
            for (u32 col = 0; col < 2; ++col) {
                acc[row][col] = round_half(acc[row][col]);
            }
        }
    }
    const u32 dest = static_cast<u32>(hmma.dest_reg.Value());
    for (u32 row = 0; row < 2; ++row) {
        X(IR::Reg{dest + row},
          ir.PackHalf2x16(ir.CompositeConstruct(acc[row][0], acc[row][1])));
    }
}

} // namespace Shader::Maxwell
