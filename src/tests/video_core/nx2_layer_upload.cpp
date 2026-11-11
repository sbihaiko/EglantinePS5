// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {
using namespace VideoCommon;
using VideoCore::Surface::PixelFormat;

void CheckStream(ImageInfo info, bool native, size_t limit, std::span<const u32> selected,
                 bool fragmented = false) {
    info.layer_stride = CalculateLayerStride(info);
    ImageInfo reference_info = info;
    reference_info.resources.layers = static_cast<s32>(*std::ranges::max_element(selected) + 1);
    const size_t guest_size = size_t{info.layer_stride} * reference_info.resources.layers;
    std::vector<u8> guest(guest_size);
    u32 random = 0x91e10da5;
    for (auto& byte : guest) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<u8>(random);
    }
    // Model GPU pages mapped to reversed device pages, including unmapped holes and
    // two GPU pages naming the same backing. The reference sees the resulting logical bytes.
    constexpr size_t page_size = 64 * 1024;
    const size_t pages = (guest.size() + page_size - 1) / page_size;
    std::vector<u8> backing;
    std::vector<size_t> page_map;
    if (fragmented) {
        backing.resize(pages * page_size);
        page_map.resize(pages);
        for (size_t page = 0; page < pages; ++page) {
            const size_t bytes = std::min(page_size, guest.size() - page * page_size);
            page_map[page] = pages - 1 - page;
            std::memcpy(backing.data() + page_map[page] * page_size,
                        guest.data() + page * page_size, bytes);
        }
        for (size_t page = 0; page < pages; ++page) {
            const size_t bytes = std::min(page_size, guest.size() - page * page_size);
            if (page % 7 == 3) {
                page_map[page] = pages;
                std::fill_n(guest.data() + page * page_size, bytes, 0);
            } else if (page % 11 == 5) {
                page_map[page] = page_map[0];
                std::memcpy(guest.data() + page * page_size,
                            backing.data() + page_map[0] * page_size, bytes);
            }
        }
    }
    std::vector<u8> expected(CalculateUnswizzledSizeBytes(reference_info));
    const auto reference = UnswizzleBlockLinearImage(reference_info, guest, expected, native);
    REQUIRE(reference.size() == static_cast<size_t>(info.resources.levels));
    for (const auto& copy : reference) {
        const size_t layer_bytes = copy.buffer_size / reference_info.resources.layers;
        for (s32 layer = 0; layer < reference_info.resources.layers; ++layer) {
            if (std::ranges::find(selected, layer) == selected.end()) {
                std::fill_n(expected.begin() + copy.buffer_offset + layer * layer_bytes,
                            layer_bytes, 0xd3);
            }
        }
    }
    std::vector<u8> actual(expected.size(), 0xd3);
    constexpr GPUVAddr base = 0x500000000ULL;
    size_t chunks{}, peak{};
    const u32 bpp = VideoCore::Surface::BytesPerBlock(info.format);
    const u32 bw = VideoCore::Surface::DefaultBlockWidth(info.format);
    const u32 bh = VideoCore::Surface::DefaultBlockHeight(info.format);
    REQUIRE(StreamLayerUploads(info, base, selected, limit,
        [&](GPUVAddr address, std::span<u8> bytes) {
            REQUIRE(address >= base);
            REQUIRE(address - base + bytes.size() <= guest.size());
            CHECK(bytes.size() <= limit);
            if (!fragmented) {
                std::memcpy(bytes.data(), guest.data() + address - base, bytes.size());
                return;
            }
            size_t offset = static_cast<size_t>(address - base);
            while (!bytes.empty()) {
                const size_t page = offset / page_size;
                const size_t in_page = offset % page_size;
                const size_t count = std::min(bytes.size(), page_size - in_page);
                if (page_map[page] == pages) std::fill_n(bytes.data(), count, 0);
                else std::memcpy(bytes.data(), backing.data() + page_map[page] * page_size +
                                 in_page, count);
                offset += count;
                bytes = bytes.subspan(count);
            }
        },
        [&](std::span<const u8> bytes, std::span<const BufferImageCopy> copies) {
            ++chunks;
            peak = std::max(peak, bytes.size());
            REQUIRE(bytes.size() <= limit);
            for (const auto& copy : copies) {
                const auto& ref = reference.at(copy.image_subresource.base_level);
                const auto layer = copy.image_subresource.base_layer;
                REQUIRE(std::ranges::find(selected, layer) != selected.end());
                REQUIRE(copy.image_subresource.num_layers == 1);
                REQUIRE(copy.image_offset.x >= 0);
                REQUIRE(copy.image_offset.y >= 0);
                REQUIRE(copy.image_offset.x % bw == 0);
                REQUIRE(copy.image_offset.y % bh == 0);
                REQUIRE(copy.image_offset.x + copy.image_extent.width <= ref.image_extent.width);
                REQUIRE(copy.image_offset.y + copy.image_extent.height <= ref.image_extent.height);
                const size_t row_bytes = ((copy.image_extent.width + bw - 1) / bw) * bpp;
                const size_t stride = ((copy.buffer_row_length + bw - 1) / bw) * bpp;
                const size_t ref_stride = ((ref.buffer_row_length + bw - 1) / bw) * bpp;
                const size_t rows = (copy.image_extent.height + bh - 1) / bh;
                const size_t dst = ref.buffer_offset + layer * (ref.buffer_size / reference_info.resources.layers) +
                                   (copy.image_offset.y / bh) * ref_stride + (copy.image_offset.x / bw) * bpp;
                for (size_t y = 0; y < rows; ++y) {
                    const size_t src = copy.buffer_offset + y * stride;
                    REQUIRE(src + row_bytes <= bytes.size());
                    REQUIRE(dst + y * ref_stride + row_bytes <= actual.size());
                    std::memcpy(actual.data() + dst + y * ref_stride, bytes.data() + src, row_bytes);
                }
            }
        }, native));
    CHECK(chunks > 0);
    CHECK(peak <= limit);
    CHECK(actual == expected);
}
}

TEST_CASE("Layer streaming preserves mip pixels, selected layer order and tiling",
          "[nx2_layer_upload]") {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.size = {777, 193, 1};
    info.format = GENERATE(PixelFormat::R8_UNORM, PixelFormat::A8B8G8R8_UNORM,
                           PixelFormat::R32G32_FLOAT, PixelFormat::BC1_RGBA_UNORM,
                           PixelFormat::BC3_UNORM);
    info.block.height = GENERATE(0U, 2U, 4U);
    info.tile_width_spacing = GENERATE(0U, 1U, 2U, 3U, 4U);
    info.resources = {5, 3};
    const bool native = GENERATE(false, true);
    CAPTURE(info.format, info.block.height, info.tile_width_spacing, native);
    CheckStream(info, native, 512 * 1024, std::array<u32, 2>{2, 0});
}

TEST_CASE("The 358 MB Mario Kart layer uses bounded upload chunks", "[nx2_layer_upload]") {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.size = {8192, 8192, 1};
    info.format = PixelFormat::A8B8G8R8_UNORM;
    info.block.height = 4;
    info.tile_width_spacing = 2;
    info.resources = {12, 1};
    CHECK(CalculateUnswizzledSizeBytes(info) == 357913920);
    CheckStream(info, true, 8 * 1024 * 1024, std::array<u32, 1>{0});
}

TEST_CASE("Mario Kart's spacing-four BC3 terrain layers stream without the 358 MB map",
          "[nx2_layer_upload]") {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.size = {8192, 8192, 1};
    info.format = PixelFormat::BC3_UNORM;
    info.block.height = 3;
    info.tile_width_spacing = 4;
    info.resources = {12, 68};
    auto layer = info;
    layer.resources.layers = 1;
    CHECK(CalculateUnswizzledSizeBytes(layer) * 4 == 357913920);
    CheckStream(info, true, 8 * 1024 * 1024, std::array<u32, 4>{0, 1, 2, 3});
}

TEST_CASE("Unsupported layer streams have no reads or partial uploads", "[nx2_layer_upload]") {
    ImageInfo good;
    good.type = ImageType::e2D;
    good.format = PixelFormat::R8_UNORM;
    good.size = {256, 256, 1};
    good.resources = {1, 1};
    size_t calls{};
    auto read = [&](GPUVAddr, std::span<u8>) { ++calls; };
    auto upload = [&](std::span<const u8>, std::span<const BufferImageCopy>) { ++calls; };
    const std::array<u32, 1> layers{0};
    for (unsigned test = 0; test < 10; ++test) {
        auto info = good;
        size_t limit = 1024 * 1024;
        switch (test) {
        case 0: info.type = ImageType::e3D; break;
        case 1: info.size.depth = 2; break;
        case 2: info.block.depth = 1; break;
        case 3: info.num_samples = 4; break;
        case 4: info.resources.levels = 0; break;
        case 5: info.resources.levels = MAX_MIP_LEVELS + 1; break;
        case 6: info.block.height = 6; break;
        case 7: limit = 511; break;
        case 8: info.layer_stride = 1; break;
        case 9: info.block.width = 1; break;
        }
        CAPTURE(test);
        CHECK_FALSE(StreamLayerUploads(info, 0x500000000ULL, layers, limit, read, upload, true));
        CHECK(calls == 0);
    }
    CHECK_FALSE(StreamLayerUploads(good, (std::numeric_limits<u64>::max)() - 10,
                                   layers, 1024 * 1024, read, upload, true));
    CHECK_FALSE(StreamLayerUploads(good, 0x500000000ULL, std::array<u32, 1>{1},
                                   1024 * 1024, read, upload, true));
    CHECK(calls == 0);
}

TEST_CASE("Layer chunks preserve pixels with scattered device pages, holes and aliases",
          "[nx2_layer_upload]") {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.size = {1536, 777, 1};
    info.format = GENERATE(PixelFormat::A8B8G8R8_UNORM, PixelFormat::BC3_UNORM);
    info.block.height = 4;
    info.tile_width_spacing = 1;
    info.resources = {4, 2};
    CheckStream(info, true, 512 * 1024, std::array<u32, 2>{1, 0}, true);
}
