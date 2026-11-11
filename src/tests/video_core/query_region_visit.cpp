// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/container/unordered_map.h"
#include "video_core/query_cache/region_visit.h"

namespace {
struct CountingHash {
    static inline size_t calls{};
    size_t operator()(u64 value) const {
        ++calls;
        return std::hash<u64>{}(value);
    }
};
using Cache = Common::unordered_map<u64, Common::unordered_map<u32, u32>, CountingHash>;
}

TEST_CASE("Large query regions visit occupied pages without a page-by-page hash scan",
          "[query_region]") {
    Cache cache;
    cache[1][0] = 1;
    cache[1000][12] = 2;
    cache[0x100000][28] = 3;
    cache[0x200000][4] = 4;
    CountingHash::calls = 0;
    std::vector<u32> hits;
    VideoCommon::VisitQueryRegion<12, false>(cache, 0, 6ULL << 30,
                                           [&](u32 id) { hits.push_back(id); });
    CHECK(hits == std::vector<u32>{1, 2, 3});
    CHECK(CountingHash::calls == 0);
    CHECK(cache.size() == 4);
}

TEST_CASE("Short query regions perform bounded page lookups and bool callbacks stop",
          "[query_region]") {
    Cache cache;
    for (u32 page = 0; page < 1000; ++page) {
        cache[page][16] = page;
        cache[page][32] = page;
    }
    CountingHash::calls = 0;
    size_t calls{};
    VideoCommon::VisitQueryRegion<12, false>(cache, 123ULL << 12, 4096,
        [&](u32 id) { CHECK(id == 123); ++calls; return true; });
    CHECK(calls == 1);
    CHECK(CountingHash::calls == 1);
    calls = 0;
    VideoCommon::VisitQueryRegion<12, false>(cache, 0, 6ULL << 30,
        [&](u32 id) { CHECK(id == 0); ++calls; return true; });
    CHECK(calls == 1);
}

TEST_CASE("Query visits preserve four-byte overlap and selective removal", "[query_region]") {
    Cache cache;
    cache[1][0] = 1;
    cache[1][4] = 2;
    cache[2][0] = 3;
    std::vector<u32> hits;
    VideoCommon::VisitQueryRegion<12, true>(cache, 4097, 1,
                                          [&](u32 id) { hits.push_back(id); });
    CHECK(hits == std::vector<u32>{1});
    CHECK_FALSE(cache.at(1).contains(0));
    CHECK(cache.at(1).at(4) == 2);
    CHECK(cache.at(2).at(0) == 3);
    VideoCommon::VisitQueryRegion<12, true>(cache, 4100, 4092,
                                          [&](u32 id) { hits.push_back(id); });
    CHECK(hits == std::vector<u32>{1, 2});
    CHECK(cache.at(2).contains(0));
}

TEST_CASE("Query region visitor matches the page-walk reference over sparse windows",
          "[query_region]") {
    Cache cache;
    for (u32 page : {0U, 1U, 3U, 14U, 38U, 111U, 500U}) {
        for (u32 offset : {0U, 4U, 172U, 4092U}) {
            cache[page][offset] = page * 4096 + offset;
        }
    }
    std::mt19937 random(0x642a);
    for (size_t trial = 0; trial < 1000; ++trial) {
        const u64 start = random() % (512 * 4096);
        const size_t size = 1 + random() % (256 * 4096);
        const u64 end = start + size;
        std::vector<u32> expected;
        for (u64 page = start >> 12; page <= (end >> 12); ++page) {
            const auto it = cache.find(page);
            if (it == cache.end()) continue;
            for (const auto& [offset, value] : it->second) {
                const u64 address = page * 4096 + offset;
                if (address < end && start < address + 4) expected.push_back(value);
            }
        }
        std::vector<u32> actual;
        VideoCommon::VisitQueryRegion<12, false>(cache, start, size,
                                               [&](u32 id) { actual.push_back(id); });
        std::ranges::sort(expected);
        std::ranges::sort(actual);
        CAPTURE(trial, start, size);
        CHECK(actual == expected);
    }
}

TEST_CASE("Empty and overflowing query windows do not invoke callbacks", "[query_region]") {
    Cache cache;
    cache[1][0] = 1;
    unsigned calls{};
    auto hit = [&](u32) { ++calls; };
    VideoCommon::VisitQueryRegion<12, false>(cache, 4097, 0, hit);
    VideoCommon::VisitQueryRegion<12, false>(cache, (std::numeric_limits<u64>::max)() - 1, 4, hit);
    CHECK(calls == 0);
}
