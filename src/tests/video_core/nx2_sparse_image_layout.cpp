// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

namespace {

VideoCommon::ImageInfo MakeImage(u32 width, u32 height) {
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::R8_UNORM;
    info.size = {width, height, 1};
    info.resources = {1, 1};
    return info;
}

} // namespace

TEST_CASE("Sparse texture byte ranges select host tiles from guest GOB columns",
          "[nx2_sparse_image]") {
    const auto info = MakeImage(256, 128);
    const std::array<std::pair<u64, u64>, 2> ranges{{{0, 1}, {1024, 1}}};
    CHECK(VideoCommon::SparseTileRects(info, ranges, 128, 64, 1) ==
          std::vector<VideoCommon::LevelRect>{{0, 0, 0, 256, 64}});
    const std::array<std::pair<u64, u64>, 1> lower{{{16384, 1}}};
    const auto rects = VideoCommon::SparseTileRects(info, lower, 128, 64, 1);
    CHECK(rects == std::vector<VideoCommon::LevelRect>{{0, 0, 64, 128, 64}});
    CHECK(VideoCommon::UnswizzledRectsBytes(info, rects) == 128 * 64);
    CHECK(VideoCommon::SparseTileRects(info, {}, 128, 64, 1).empty());
}

TEST_CASE("Sparse texture edge tiles clip to the image width", "[nx2_sparse_image]") {
    const auto info = MakeImage(130, 65);
    const std::array<std::pair<u64, u64>, 1> edge{{{1024, 1}}};
    const auto rects = VideoCommon::SparseTileRects(info, edge, 128, 64, 1);
    CHECK(rects == std::vector<VideoCommon::LevelRect>{{0, 128, 0, 2, 64}});
    CHECK(VideoCommon::UnswizzledRectsBytes(info, rects) == 64 * 64);
    const std::array<std::pair<u64, u64>, 1> next_row{{{1536, 512}}};
    CHECK(VideoCommon::SparseTileRects(info, next_row, 128, 64, 1) ==
          std::vector<VideoCommon::LevelRect>{{0, 0, 0, 128, 64}});
}

TEST_CASE("Sparse mip tails upload whole levels only when touched", "[nx2_sparse_image]") {
    auto info = MakeImage(256, 128);
    info.resources.levels = 3;
    const auto offsets = VideoCommon::CalculateMipLevelOffsets(info);
    const std::array<std::pair<u64, u64>, 1> tail{{{offsets[2] + 1, 1}}};
    CHECK(VideoCommon::SparseTileRects(info, tail, 128, 64, 1) ==
          std::vector<VideoCommon::LevelRect>{{2, 0, 0, 64, 32}});
    const std::array<std::pair<u64, u64>, 1> boundary{{{offsets[1] - 1, 2}}};
    const auto rects = VideoCommon::SparseTileRects(info, boundary, 128, 64, 1);
    REQUIRE(rects.size() == 2);
    CHECK(rects.back() == VideoCommon::LevelRect{1, 0, 0, 128, 64});
}

TEST_CASE("Sparse rectangle uploads match the whole-image unswizzle bytes and regions",
          "[nx2_sparse_image][nx2_sparse_unswizzle]") {
    using VideoCore::Surface::PixelFormat;
    const auto format = GENERATE(PixelFormat::R8_UNORM, PixelFormat::R16_UNORM,
                                 PixelFormat::A8B8G8R8_UNORM, PixelFormat::R32G32_FLOAT,
                                 PixelFormat::R32G32B32A32_FLOAT);
    const auto block_height = GENERATE(0U, 2U, 4U);
    const auto spacing = GENERATE(0U, 1U, 2U, 3U);
    const bool native_tiling = GENERATE(false, true);
    CAPTURE(format, block_height, spacing, native_tiling);
    auto info = MakeImage(258, 137);
    info.format = format;
    info.block.height = block_height;
    info.tile_width_spacing = spacing;
    info.resources.levels = 5;
    const u32 bytes_per_pixel = VideoCore::Surface::BytesPerBlock(format);
    std::vector<u8> guest(VideoCommon::CalculateGuestSizeInBytes(info));
    u32 random = 0x91e10da5;
    for (auto& byte : guest) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<u8>(random);
    }
    std::vector<u8> whole(VideoCommon::CalculateUnswizzledSizeBytes(info));
    const auto whole_copies =
        VideoCommon::UnswizzleBlockLinearImage(info, guest, whole, native_tiling);
    REQUIRE(whole_copies.size() == 5);
    std::vector<VideoCommon::LevelRect> rects{{0, 128, 64, 128, 64}, {0, 256, 128, 2, 9}};
    for (s32 level = 1; level < info.resources.levels; ++level) {
        const auto& extent = whole_copies[level].image_extent;
        rects.push_back({level, 0, 0, extent.width, extent.height});
    }

    const auto output_size = VideoCommon::UnswizzledRectsBytes(info, rects);
    constexpr size_t guard_size = 32;
    std::vector<u8> guarded_output(output_size + 2 * guard_size, 0xa5);
    const auto output = std::span{guarded_output}.subspan(guard_size, output_size);
    constexpr GPUVAddr guest_base = 0x123450000;
    size_t reads = 0;
    const auto copies = VideoCommon::UnswizzleImageRects(
        [&](GPUVAddr address, std::span<u8> bytes) {
            REQUIRE(address >= guest_base);
            const auto offset = address - guest_base;
            REQUIRE(offset <= guest.size());
            REQUIRE(bytes.size() <= guest.size() - offset);
            std::memcpy(bytes.data(), guest.data() + offset, bytes.size());
            ++reads;
        },
        guest_base, info, rects, output, native_tiling);
    REQUIRE_FALSE(copies.empty());
    CHECK(reads == copies.size());
    CHECK(std::ranges::all_of(std::span{guarded_output}.first(guard_size),
                              [](u8 byte) { return byte == 0xa5; }));
    CHECK(std::ranges::all_of(std::span{guarded_output}.last(guard_size),
                              [](u8 byte) { return byte == 0xa5; }));

    std::vector<u8> actual(whole.size(), 0xcd);
    std::vector<u8> expected(whole.size(), 0xcd);
    std::vector<u8> actual_coverage(whole.size());
    std::vector<u8> expected_coverage(whole.size());
    for (const auto& rect : rects) {
        const auto& whole_copy = whole_copies[rect.level];
        for (u32 y = rect.y; y < rect.y + rect.height; ++y) {
            const size_t offset = whole_copy.buffer_offset +
                                  (size_t{y} * whole_copy.buffer_row_length + rect.x) *
                                      bytes_per_pixel;
            const size_t bytes = size_t{rect.width} * bytes_per_pixel;
            std::copy_n(whole.begin() + offset, bytes, expected.begin() + offset);
            std::fill_n(expected_coverage.begin() + offset, bytes, 1);
        }
    }
    size_t consumed_bytes = 0;
    for (const auto& copy : copies) {
        const auto level = copy.image_subresource.base_level;
        REQUIRE(level >= 0);
        REQUIRE(level < info.resources.levels);
        const auto& whole_copy = whole_copies[level];
        CHECK(copy.image_subresource.base_layer == 0);
        CHECK(copy.image_subresource.num_layers == 1);
        CHECK(copy.image_offset.z == 0);
        CHECK(copy.image_extent.depth == 1);
        REQUIRE(copy.image_offset.x >= 0);
        REQUIRE(copy.image_offset.y >= 0);
        REQUIRE(copy.image_offset.x + copy.image_extent.width <= whole_copy.image_extent.width);
        REQUIRE(copy.image_offset.y + copy.image_extent.height <= whole_copy.image_extent.height);
        REQUIRE(copy.buffer_row_length >= copy.image_extent.width);
        REQUIRE(copy.buffer_image_height >= copy.image_extent.height);
        CHECK(copy.buffer_size == size_t{copy.buffer_row_length} * copy.buffer_image_height *
                                      bytes_per_pixel);
        REQUIRE(copy.buffer_offset >= consumed_bytes);
        REQUIRE(copy.buffer_offset < consumed_bytes + copy.buffer_size);
        consumed_bytes += copy.buffer_size;
        for (u32 row = 0; row < copy.image_extent.height; ++row) {
            const size_t source = copy.buffer_offset +
                                  size_t{row} * copy.buffer_row_length * bytes_per_pixel;
            const size_t destination =
                whole_copy.buffer_offset +
                (size_t{copy.image_offset.y + row} * whole_copy.buffer_row_length +
                 copy.image_offset.x) * bytes_per_pixel;
            const size_t bytes = size_t{copy.image_extent.width} * bytes_per_pixel;
            REQUIRE(source + bytes <= output.size());
            REQUIRE(destination + bytes <= actual.size());
            CHECK(std::equal(output.begin() + source, output.begin() + source + bytes,
                             whole.begin() + destination));
            std::copy_n(output.begin() + source, bytes, actual.begin() + destination);
            for (size_t byte = destination; byte < destination + bytes; ++byte) {
                ++actual_coverage[byte];
            }
        }
    }
    CHECK(consumed_bytes == output_size);
    CHECK(actual_coverage == expected_coverage);
    CHECK(actual == expected);

    std::fill(actual.begin(), actual.end(), 0xcd);
    std::fill(actual_coverage.begin(), actual_coverage.end(), 0);
    constexpr size_t chunk_limit = 32768;
    size_t chunks{};
    REQUIRE(VideoCommon::StreamRectUploads(info, guest_base, rects, chunk_limit,
        [&](GPUVAddr address, std::span<u8> bytes) {
            REQUIRE(address >= guest_base);
            REQUIRE(address - guest_base + bytes.size() <= guest.size());
            REQUIRE(bytes.size() <= chunk_limit);
            std::memcpy(bytes.data(), guest.data() + address - guest_base, bytes.size());
        },
        [&](std::span<const u8> bytes, std::span<const VideoCommon::BufferImageCopy> uploads) {
            ++chunks;
            REQUIRE(bytes.size() <= chunk_limit);
            for (const auto& copy : uploads) {
                const auto& ref = whole_copies.at(copy.image_subresource.base_level);
                REQUIRE(copy.image_subresource.base_layer == 0);
                REQUIRE(copy.image_subresource.num_layers == 1);
                for (u32 y = 0; y < copy.image_extent.height; ++y) {
                    const size_t source = copy.buffer_offset + size_t{y} *
                                          copy.buffer_row_length * bytes_per_pixel;
                    const size_t destination = ref.buffer_offset +
                        (size_t{copy.image_offset.y + y} * ref.buffer_row_length +
                         copy.image_offset.x) * bytes_per_pixel;
                    const size_t row_bytes = size_t{copy.image_extent.width} * bytes_per_pixel;
                    REQUIRE(source + row_bytes <= bytes.size());
                    REQUIRE(destination + row_bytes <= actual.size());
                    std::copy_n(bytes.begin() + source, row_bytes, actual.begin() + destination);
                    for (size_t byte = destination; byte < destination + row_bytes; ++byte)
                        ++actual_coverage[byte];
                }
            }
        }, native_tiling));
    CHECK(chunks > 0);
    CHECK(actual_coverage == expected_coverage);
    CHECK(actual == expected);
}

TEST_CASE("Spaced sparse tile selection covers every texel from written guest ranges",
          "[nx2_sparse_image][nx2_sparse_unswizzle]") {
    using VideoCore::Surface::PixelFormat;
    const auto spacing = GENERATE(1U, 2U, 3U);
    const auto format = GENERATE(PixelFormat::R8_UNORM, PixelFormat::R16_UNORM);
    const bool native = GENERATE(false, true);
    auto info = MakeImage(777, 193);
    info.format = format;
    info.block.height = 2;
    info.tile_width_spacing = spacing;
    info.resources.levels = 4;
    const size_t guest_size = VideoCommon::CalculateGuestSizeInBytes(info);
    for (const auto first : {size_t{0},size_t{512},size_t{8192},guest_size/2,guest_size-512}) {
        CAPTURE(spacing,format,native,first);
        const size_t bytes = std::min(size_t{512},guest_size-first);
        std::vector<u8> guest(guest_size);
        std::fill_n(guest.begin()+first,bytes,0xff);
        std::vector<u8> whole(VideoCommon::CalculateUnswizzledSizeBytes(info));
        const auto copies = VideoCommon::UnswizzleBlockLinearImage(info,guest,whole,native);
        const std::array<std::pair<u64,u64>,1> ranges{{{first,bytes}}};
        const auto rects=VideoCommon::SparseTileRects(info,ranges,128,64,3);
        const u32 bpp=VideoCore::Surface::BytesPerBlock(format);
        for(size_t level=0;level<copies.size();++level) {
            const auto& c=copies[level];
            for(u32 y=0;y<c.image_extent.height;++y)for(u32 x=0;x<c.image_extent.width;++x) {
                const size_t offset=c.buffer_offset+(size_t{y}*c.buffer_row_length+x)*bpp;
                if(!std::ranges::any_of(std::span{whole}.subspan(offset,bpp),[](u8 v){return v!=0;}))continue;
                REQUIRE(std::ranges::any_of(rects,[&](const auto& r){
                    return r.level==level && x>=r.x && x<r.x+r.width && y>=r.y && y<r.y+r.height;
                }));
            }
        }
    }
}

TEST_CASE("Banded readback preserves pixels and padded guest bytes", "[nx2_sparse_image]") {
    const auto spacing=GENERATE(0U,1U,2U,3U);
    const auto block_height=GENERATE(0U,2U,4U);
    const auto format=GENERATE(VideoCore::Surface::PixelFormat::R8_UNORM,
                              VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM);
    const bool native=GENERATE(false,true);
    auto info=MakeImage(777,193);info.format=format;info.tile_width_spacing=spacing;
    info.block.height=block_height;
    std::vector<u8> initial(VideoCommon::CalculateGuestSizeInBytes(info));
    for(size_t i=0;i<initial.size();++i)initial[i]=u8(i*39+11);
    std::vector<u8> linear(VideoCommon::CalculateUnswizzledSizeBytes(info));
    for(size_t i=0;i<linear.size();++i)linear[i]=u8(i*73+19);
    auto expected=initial;
    const u32 bpp=VideoCore::Surface::BytesPerBlock(format);
    // Whole-image baseline through the same decoder's full-layout writer.
    const u32 alignment=VideoCommon::CalculateLevelStrideAlignment(info,0);
    Tegra::Texture::SwizzleTexture(expected,linear,bpp,info.size.width,info.size.height,1,
                                   block_height,0,alignment,native);
    auto actual=initial;
    const auto bands=VideoCommon::DownloadRowBands(info,512*1024);
    REQUIRE_FALSE(bands.empty());
    size_t seen=0;
    for(const auto& copy:bands){
        CHECK(copy.buffer_size<=512*1024);
        const auto [offset,bytes]=VideoCommon::RowBandGuestRange(info,copy);
        REQUIRE(offset+bytes<=actual.size());
        const size_t host=size_t(copy.image_offset.y)*info.size.width*bpp;
        VideoCommon::SwizzleRowBand(info,copy,std::span{linear}.subspan(host,copy.buffer_size),
                                    std::span{actual}.subspan(offset,bytes),native);
        seen+=copy.image_extent.height;
    }
    CHECK(seen==info.size.height);
    CHECK(actual==expected);
}

TEST_CASE("Sparse upload preflight rejects invalid rectangles without partial callbacks",
          "[nx2_sparse_image][nx2_sparse_unswizzle]") {
    auto info = MakeImage(256, 256);
    info.block.height = 2;
    size_t callbacks{};
    const auto read = [&](GPUVAddr, std::span<u8>) { ++callbacks; };
    const auto upload = [&](std::span<const u8>, std::span<const VideoCommon::BufferImageCopy>) {
        ++callbacks;
    };
    for (const auto rect : std::array<VideoCommon::LevelRect, 6>{
             {{1,0,0,16,16}, {0,0,0,0,16}, {0,0,0,257,16},
              {0,0,250,64,16}, {0,1,0,64,16}, {-1,0,0,64,16}}}) {
        const std::array rects{VideoCommon::LevelRect{0,0,0,64,16}, rect};
        CHECK_FALSE(VideoCommon::StreamRectUploads(info, 0x500000000, rects, 32768,
                                                   read, upload, true));
        CHECK(callbacks == 0);
    }
    const std::array rects{VideoCommon::LevelRect{0,0,0,64,16}};
    CHECK_FALSE(VideoCommon::StreamRectUploads(info, 0x500000000, rects, 1, read, upload, true));
    CHECK_FALSE(VideoCommon::StreamRectUploads(info, ~GPUVAddr{}, rects, 32768,
                                               read, upload, true));
    CHECK(callbacks == 0);
}
