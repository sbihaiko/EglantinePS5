// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/assert.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

template <typename T, typename Id = u32>
class DescriptorTable {
public:
    [[nodiscard]] bool Synchronize(GPUVAddr gpu_addr, u32 limit) noexcept {
        bool ret = !(current_gpu_addr == gpu_addr && current_limit == limit);
        if (ret) {
            Refresh(gpu_addr, limit);
        }
        return ret;
    }

    void Invalidate() noexcept {
        // Every deleted image invalidates every channel's tables: clear once, on the next read.
        invalidated = true;
        ++changes;
    }

    [[nodiscard]] std::pair<T, bool> Read(Tegra::MemoryManager const& gpu_memory, u32 index) noexcept {
        DEBUG_ASSERT(index <= current_limit);
        const GPUVAddr gpu_addr = current_gpu_addr + index * sizeof(T);
        std::pair<T, bool> result;
        gpu_memory.ReadBlockUnsafe(gpu_addr, std::addressof(result.first), sizeof(T));
        if (invalidated) {
            std::ranges::fill(read_descriptors, 0);
            invalidated = false;
        }
        auto& bits = read_descriptors[index / 64];
        const u64 mask = 1ULL << (index % 64);
        result.second = (bits & mask) == 0 || result.first != descriptors[index];
        bits |= mask;
        if (result.second) {
            descriptors[index] = result.first;
        }
        return result;
    }

    void Refresh(GPUVAddr gpu_addr, u32 limit) noexcept {
        current_gpu_addr = gpu_addr;
        current_limit = limit;
        ++changes;
        // Power-of-two growth: amortised like the old 0x80000 rounding without 16 MiB per table.
        const size_t num_descriptors = std::bit_ceil(static_cast<size_t>(limit) + 1);
        size_t old_size = read_descriptors.size();
        read_descriptors.resize(Common::DivCeil(num_descriptors, 64U));
        old_size = (std::min)(old_size, read_descriptors.size());
        std::fill(read_descriptors.begin(), read_descriptors.begin() + old_size, 0);
        invalidated = false;
        if (descriptors.size() < num_descriptors) {
            descriptors.resize(num_descriptors);
            bound_ids.resize(num_descriptors);
        }
    }

    std::vector<u64> read_descriptors;
    std::vector<T> descriptors;
    std::vector<Id> bound_ids; ///< Cache id resolved for each read descriptor
    GPUVAddr current_gpu_addr{};
    u32 current_limit{};
    bool invalidated{};
    /// Bumped by every Invalidate and Refresh: the ids resolved before are only good while it
    /// stays the same (texture memo).
    u64 changes{};
};

} // namespace VideoCommon
