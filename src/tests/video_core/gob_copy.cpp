// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "video_core/textures/decoders.h"

TEST_CASE("Whole GOB copies preserve the unchanged scalar subrect layout",
          "[texture][gob_copy]") {
    using namespace Tegra::Texture;
    const u32 bpp = GENERATE(1U, 2U, 4U, 8U, 16U);
    const u32 width = GENERATE(16U, 64U, 128U, 258U);
    const u32 height = GENERATE(1U, 9U, 65U);
    const u32 block_height = GENERATE(0U, 4U);
    const u32 block_depth = GENERATE(0U, 1U);
    const bool native = GENERATE(false, true);
    const u32 depth = block_depth ? 3 : 1;
    CAPTURE(bpp, width, height, block_height, block_depth, native);
    const size_t linear_bytes = size_t{width} * height * depth * bpp;
    const size_t tiled_bytes = CalculateSize(true, bpp, width, height, depth, block_height, block_depth);
    std::vector<u8> input(linear_bytes), expected(tiled_bytes + 64, 0xa5), tiled(tiled_bytes + 64, 0xa5);
    u32 random = 0x3175a91d;
    for (auto& value : input) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        value = static_cast<u8>(random);
    }
    SwizzleSubrect(std::span{expected}.first(tiled_bytes), input, bpp, width, height, depth,
                   0, 0, width, height * depth, block_height, block_depth, width * bpp, native);
    SwizzleTexture(std::span{tiled}.first(tiled_bytes), input, bpp, width, height, depth,
                   block_height, block_depth, 0, native);
    REQUIRE(tiled == expected);
    std::vector<u8> result(linear_bytes + 64, 0xa5);
    UnswizzleTexture(std::span{result}.first(linear_bytes), expected, bpp, width, height, depth,
                     block_height, block_depth, 0, native);
    REQUIRE(std::equal(input.begin(), input.end(), result.begin()));
    REQUIRE(std::ranges::all_of(std::span{result}.last(64), [](u8 value) { return value == 0xa5; }));
}
