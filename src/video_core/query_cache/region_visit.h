// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <type_traits>
#include <vector>
#include "common/common_types.h"

namespace VideoCommon {

/// The caller holds the cache lock. Visit pages in address order, choosing the smaller of the
/// requested page interval and the occupied-page table. A large texture flush can span gigabytes
/// while the query cache contains only a few pages.
template <unsigned PageBits, bool Remove, typename Cache, typename Func>
void VisitQueryRegion(Cache& cache, u64 address, size_t size, Func&& func) {
    static_assert(PageBits > 0 && PageBits < 64);
    using Location = typename Cache::mapped_type::mapped_type;
    constexpr bool ReturnsBool = std::is_same_v<std::invoke_result_t<Func&, Location>, bool>;
    if (size == 0 || size > (std::numeric_limits<u64>::max)() - address || cache.empty()) {
        return;
    }
    const u64 end = address + size;
    const u64 first_page = address >> PageBits;
    const u64 last_page = (end - 1) >> PageBits;
    const auto visit = [&](auto& entry) {
        const u64 page_start = entry.first << PageBits;
        const auto in_range = [&](u32 offset) {
            const u64 begin = page_start + offset;
            return begin < end && (begin >= address || address - begin < sizeof(u32));
        };
        auto& contents = entry.second;
        for (auto& query : contents) {
            if (!in_range(query.first)) {
                continue;
            }
            if constexpr (ReturnsBool) {
                if (std::invoke(func, query.second)) {
                    return true;
                }
            } else {
                std::invoke(func, query.second);
            }
        }
        if constexpr (Remove) {
            erase_if(contents, [&](const auto& query) { return in_range(query.first); });
        }
        return false;
    };
    if (last_page - first_page + 1 <= cache.size()) {
        for (u64 page = first_page; page <= last_page; ++page) {
            auto it = cache.find(page);
            if (it != cache.end() && visit(*it)) {
                return;
            }
        }
        return;
    }
    std::vector<typename Cache::value_type*> pages;
    for (auto& entry : cache) {
        if (entry.first >= first_page && entry.first <= last_page) {
            pages.push_back(&entry);
        }
    }
    std::ranges::sort(pages, {}, [](const auto* entry) { return entry->first; });
    for (auto* entry : pages) {
        if (visit(*entry)) {
            return;
        }
    }
}

} // namespace VideoCommon
