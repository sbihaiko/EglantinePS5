// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

namespace {
using namespace VideoCommon;
using VideoCore::Surface::PixelFormat;
void CheckPartial(ImageInfo info, bool native, u64 offset, size_t size) {
    const size_t guest_size = CalculateGuestSizeInBytes(info);
    REQUIRE(offset + size <= guest_size);
    std::vector<u8> initial(guest_size), linear(CalculateUnswizzledSizeBytes(info));
    u32 random = 0x948fa513;
    for (auto* buffer : {&initial, &linear}) for (auto& value : *buffer) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        value = static_cast<u8>(random);
    }
    auto whole = initial;
    const auto bpp = VideoCore::Surface::BytesPerBlock(info.format);
    Tegra::Texture::SwizzleTexture(whole, linear, bpp, info.size.width, info.size.height, 1,
        info.block.height, 0, CalculateLevelStrideAlignment(info, 0), native);
    auto expected = initial;
    std::copy_n(whole.begin() + offset, size, expected.begin() + offset);
    auto actual = initial;
    const auto bands = PartialDownloadBands(info, offset, size, 4 * 1024 * 1024);
    REQUIRE(bands.has_value());
    size_t written{};
    for (const auto& band : *bands) {
        REQUIRE(band.guest_size <= 4 * 1024 * 1024);
        REQUIRE(band.copy.buffer_size <= 4 * 1024 * 1024);
        REQUIRE(band.guest_offset + band.guest_size <= guest_size);
        REQUIRE(band.write_offset >= offset);
        REQUIRE(band.write_offset + band.write_size <= offset + size);
        REQUIRE(band.write_offset >= band.guest_offset);
        REQUIRE(band.write_offset + band.write_size <= band.guest_offset + band.guest_size);
        std::vector<u8> guarded(band.guest_size + 64, 0xa5);
        const auto guest = std::span{guarded}.subspan(32, band.guest_size);
        std::copy_n(initial.begin() + band.guest_offset, band.guest_size, guest.begin());
        const size_t source = size_t{static_cast<u32>(band.copy.image_offset.y)} * info.size.width * bpp;
        REQUIRE(source + band.copy.buffer_size <= linear.size());
        SwizzleRowBand(info, band.copy, std::span{linear}.subspan(source, band.copy.buffer_size),
                       guest, native);
        std::copy_n(guest.begin() + band.write_offset - band.guest_offset, band.write_size,
                    actual.begin() + band.write_offset);
        CHECK(std::ranges::all_of(std::span{guarded}.first(32), [](u8 b) { return b == 0xa5; }));
        CHECK(std::ranges::all_of(std::span{guarded}.last(32), [](u8 b) { return b == 0xa5; }));
        written += band.write_size;
    }
    CHECK(written <= size); // Padding not occupied by image texels needs no GPU transfer.
    CHECK(actual == expected); // Includes every untouched byte outside the request.
}
}

TEST_CASE("Partial texture downloads match full readback only inside the requested bytes",
          "[texture][partial_download]") {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.size = {777, 193, 1};
    info.resources = {1, 1};
    info.format = GENERATE(PixelFormat::R8_UNORM, PixelFormat::R16_UNORM,
                           PixelFormat::A8B8G8R8_UNORM, PixelFormat::R32G32_FLOAT);
    info.block.height = GENERATE(0U, 2U, 4U);
    info.tile_width_spacing = GENERATE(0U, 2U, 4U);
    const bool native = GENERATE(false, true);
    info.layer_stride = CalculateLayerStride(info);
    const size_t guest_bytes = CalculateGuestSizeInBytes(info);
    CAPTURE(info.format, info.block.height, info.tile_width_spacing, native);
    CheckPartial(info, native, 0, 64);
    CheckPartial(info, native, 7, 4096);
    CheckPartial(info, native, guest_bytes / 2 - 97, 4096);
    CheckPartial(info, native, guest_bytes - 100, 100);
}

TEST_CASE("A command read into a 64MiB image transfers a bounded GOB row",
          "[texture][partial_download]") {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.size = {4096, 4096, 1};
    info.resources = {1, 1};
    info.format = PixelFormat::A8B8G8R8_UNORM;
    info.block.height = 4;
    info.layer_stride = CalculateLayerStride(info);
    const auto bands = PartialDownloadBands(info, 0x1234, 4096, 4 * 1024 * 1024);
    REQUIRE(bands.has_value());
    REQUIRE(bands->size() == 1);
    CHECK(bands->front().copy.buffer_size == 2 * 1024 * 1024);
    CHECK(bands->front().write_size == 4096);
    CheckPartial(info, true, 0x1234, 4096);
}

TEST_CASE("Unsupported partial downloads preserve the full-image fallback",
          "[texture][partial_download]") {
    ImageInfo info;
    info.type = ImageType::e2D;
    info.size = {256, 256, 1};
    info.resources = {1, 1};
    info.format = PixelFormat::R8_UNORM;
    info.block.height = 2;
    const size_t bytes = CalculateGuestSizeInBytes(info);
    CHECK_FALSE(PartialDownloadBands(info, 0, bytes, 4 * 1024 * 1024));
    CHECK_FALSE(PartialDownloadBands(info, 0, 0, 4 * 1024 * 1024));
    CHECK_FALSE(PartialDownloadBands(info, bytes - 1, 2, 4 * 1024 * 1024));
    CHECK_FALSE(PartialDownloadBands(info, std::numeric_limits<u64>::max(), 2, 4 * 1024 * 1024));
    CHECK_FALSE(PartialDownloadBands(info, 0, 4, 1));
    info.resources.layers = 2;
    CHECK_FALSE(PartialDownloadBands(info, 0, 4, 4 * 1024 * 1024));
}
