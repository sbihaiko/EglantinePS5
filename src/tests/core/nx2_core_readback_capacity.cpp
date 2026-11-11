// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/memory/core_readback_cache.h"

namespace {
using Core::Memory::CoreReadbackCacheImpl;
using LargerReadbackCache = CoreReadbackCacheImpl<10>;
using Core::Memory::HostReadbackCache;
using Core::Memory::YUZU_PAGESIZE;
}

TEST_CASE("NX2 core readback cache retains a 128-page certified working set",
          "[core][nx2][readback_cache]") {
    std::vector<u8> backing(128 * YUZU_PAGESIZE);
    auto cache = std::make_unique<LargerReadbackCache>();
    CoreReadbackCacheImpl<5> original;
    std::array<HostReadbackCache::Page, 128> pages{};
    for (size_t index = 0; index < pages.size(); ++index) {
        auto& page = pages[index];
        page = {&backing, backing.data() + index * YUZU_PAGESIZE,
                (index + 1) * YUZU_PAGESIZE, 3, 1};
        const VideoCore::RasterizerDownloadArea area{
            page.device_page, page.device_page + YUZU_PAGESIZE, false, 11};
        REQUIRE(cache->Remember(page, area, page, 1, 11));
        REQUIRE(original.Remember(page, area, page, 1, 11));
    }
    size_t original_hits = 0;
    for (const auto& page : pages) {
        REQUIRE(cache->CanReuse(page, 1, 11, 4, 4));
        original_hits += original.CanReuse(page, 1, 11, 4, 4);
    }
    REQUIRE(original_hits <= 64);
}

TEST_CASE("NX2 larger core cache retains identity and generation guards",
          "[core][nx2][readback_cache]") {
    std::array<u8, YUZU_PAGESIZE * 2> backing{};
    const HostReadbackCache::Page page{&backing, backing.data(), 0x8000, 3, 1};
    const VideoCore::RasterizerDownloadArea area{0x8000, 0x9000, false, 11};
    auto cache = std::make_unique<LargerReadbackCache>();
    auto changed = page;
    SECTION("mapping epoch") { ++changed.trap_generation; }
    SECTION("session identity") { ++changed.owner_instance; }
    SECTION("host backing") { changed.host_page += YUZU_PAGESIZE; }
    SECTION("device alias") { changed.device_page += YUZU_PAGESIZE; }
    SECTION("owner identity") { changed.owner = &cache; }
    REQUIRE(cache->Remember(page, area, page, 1, 11));
    REQUIRE_FALSE(cache->CanReuse(changed, 1, 11, 4, 4));
    for (const u64 generation : {0ULL, 12ULL}) {
        REQUIRE(cache->Remember(page, area, page, 1, 11));
        REQUIRE_FALSE(cache->CanReuse(page, 1, generation, 4, 4));
    }
    for (const size_t aliases : {size_t{0}, size_t{2}}) {
        REQUIRE(cache->Remember(page, area, page, 1, 11));
        REQUIRE_FALSE(cache->CanReuse(page, aliases, 11, 4, 4));
    }
}

TEST_CASE("NX2 larger core cache rejects incomplete certificates",
          "[core][nx2][readback_cache]") {
    std::array<u8, YUZU_PAGESIZE> backing{};
    const HostReadbackCache::Page page{&backing, backing.data(), 0x8000, 3, 1};
    auto cache = std::make_unique<LargerReadbackCache>();
    for (const VideoCore::RasterizerDownloadArea area : std::array{
             VideoCore::RasterizerDownloadArea{0x8001, 0x9000, true, 11},
             VideoCore::RasterizerDownloadArea{0x8000, 0x8fff, true, 11},
             VideoCore::RasterizerDownloadArea{0x8000, 0x9000, true, 0},
             VideoCore::RasterizerDownloadArea{0x8000, 0x9000, true, 12}}) {
        REQUIRE_FALSE(cache->Remember(page, area, page, 1, 11));
        REQUIRE_FALSE(cache->CanReuse(page, 1, 11, 0, 4));
    }
}

TEST_CASE("Core ordered reads certify a mapped page without requiring a fastmem arena",
          "[core][nx2][readback_cache]") {
    std::array<u8, YUZU_PAGESIZE * 2> backing{};
    const HostReadbackCache::Page page{&backing, backing.data(), 0x8000, 3, 1};
    CoreReadbackCacheImpl<5> cache;
    for (const u64 offset : {0ULL, 4ULL, 2048ULL, 4092ULL}) {
        const auto request = cache.OrderedRange(page, 1, 0x8000 + offset, 4);
        REQUIRE(request.address == 0x8000);
        REQUIRE(request.size == YUZU_PAGESIZE);
        const VideoCore::RasterizerDownloadArea completed{
            request.address, request.address + request.size, false, 11};
        REQUIRE(cache.Remember(page, completed, page, 1, 11));
        REQUIRE(cache.CanReuse(page, 1, 11, offset, 4));
        REQUIRE_FALSE(cache.CanReuse(page, 1, 12, offset, 4));
    }
    for (unsigned invalid = 0; invalid < 9; ++invalid) {
        auto changed = page;
        size_t aliases = 1;
        DAddr address = 0x8004;
        u64 bytes = 4;
        switch (invalid) {
        case 0: changed.owner = nullptr; break;
        case 1: changed.owner_instance = 0; break;
        case 2: changed.host_page = nullptr; break;
        case 3: changed.device_page = 0; break;
        case 4: changed.device_page += 1; break;
        case 5: aliases = 2; break;
        case 6: bytes = YUZU_PAGESIZE; break;
        case 7: address += YUZU_PAGESIZE; break;
        case 8: bytes = 0; break;
        }
        const auto request = cache.OrderedRange(changed, aliases, address, bytes);
        REQUIRE(request.address == address);
        REQUIRE(request.size == bytes);
    }
}
