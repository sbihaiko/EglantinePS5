// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_override.h"

TEST_CASE("Ampere ordinary gathers pack half results and reject the other precision modes",
          "[video_core][ampere][release_guards]") {
    {
        // Bit 79 (.F16), once rejected as unproved: upstream Eglantine lowers it (its Welcome Tour
        // and Ocarina of Time captures), and Ocarina of Time's gameplay upscaler needs it. The four
        // taps are converted to halves and packed in pairs into Rd (R2) and the second
        // destination (R1), the first of each pair in the low half.
        const std::array<Shader::Ampere::Instruction, 2> code{{
            {{0x06027b63, 0x33c04407, 0x041e0f01U | (1U << 15), 0x000fc200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto pack = [](u32 target) {
            return 0x5c18000000070000ULL | target | (247ULL << 8) | (246ULL << 20) | (16ULL << 39);
        };
        REQUIRE(std::count(lowered.instructions.begin(), lowered.instructions.end(), pack(2)) == 1);
        REQUIRE(std::count(lowered.instructions.begin(), lowered.instructions.end(), pack(1)) == 1);
        // Nothing but the two packs writes a guest register.
        for (const u64 word : lowered.instructions) {
            if ((word >> 52) == 0xe30 || word == pack(2) || word == pack(1)) continue;
            REQUIRE((word & 0xff) >= 246);
        }
    }
    for (const u32 precision : {2U, 3U}) {
        const std::array<Shader::Ampere::Instruction, 2> code{{
            {{0x06027b63, 0x33c04407, 0x041e0f01U | (precision << 15), 0x000fc200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error == "native texture gather mode");
        REQUIRE(lowered.error_offset == 0);
        REQUIRE(lowered.instructions.empty());
        REQUIRE(lowered.source_instruction_offsets.empty());
    }
}

TEST_CASE("Ampere IADD3.X with a third source rejects carry outputs and a second carry input",
          "[video_core][ampere][release_guards]") {
    // IADD3.X R1, R1, RZ, R2, P0
    constexpr u32 plain = 0x007fe402;
    const auto lower = [](u32 word2) {
        const std::array<Shader::Ampere::Instruction, 2> code{{
            {{0x01017210, 0xff, word2, 0x000fc800}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        return Shader::Ampere::LowerToMaxwell(code);
    };
    REQUIRE(static_cast<bool>(lower(plain)));
    for (const u32 word2 : {(plain & ~(7U << 17)) | (1U << 17),   // carry out P1
                            (plain & ~(7U << 20)) | (1U << 20)}) { // second carry out P1
        const auto lowered = lower(word2);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error == "native integer carry mode");
    }
    {
        // A second carry input (P1 beside P0) with no carry output was rejected too. Ocarina of
        // Time's foliage programs have it (IADD3.X R7, RZ, R1, RZ, P1, P2), and upstream Eglantine
        // lowers it: the sum builds up in R253 and each carry is added as a 0/1 selected by its
        // predicate.
        const auto lowered = lower((plain & ~(15U << 13)) | (1U << 13));
        INFO(lowered.error);
        REQUIRE(static_cast<bool>(lowered));
        const auto selects = [&](u32 predicate) {
            return std::count_if(lowered.instructions.begin(), lowered.instructions.end(), [&](u64 word) {
                return (word >> 51) == (0x5ca0000000000000ULL >> 51) && ((word >> 39) & 7) == predicate;
            });
        };
        REQUIRE(selects(0) == 1);
        REQUIRE(selects(1) == 1);
        // The sum is copied to R1 last; nothing else writes a guest register.
        for (const u64 word : lowered.instructions) {
            if ((word >> 52) == 0xe30) continue;
            REQUIRE(((word & 0xff) >= 246 || (word & 0xff) == 1));
        }
    }
}

TEST_CASE("Ampere program overrides validate the surface buffer",
          "[video_core][ampere][release_guards]") {
    const std::array<Shader::Ampere::Instruction, 1> code{{{{0x0000794d, 0, 0x03800000, 0}}}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    auto bytes = Shader::Ampere::EncodeProgramOverride(code, lowered);
    REQUIRE(Shader::Ampere::ProgramOverrideSourceSize(bytes) == sizeof(code));
    for (const u32 surface : {3U, 18U}) {
        std::memcpy(bytes.data() + 6 * sizeof(u32), &surface, sizeof(surface));
        REQUIRE(static_cast<bool>(Shader::Ampere::DecodeProgramOverride(bytes, code)) == (surface < 18));
    }
}
