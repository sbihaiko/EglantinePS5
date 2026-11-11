// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace VideoCommon {
// Allocation policy only: a preference never certifies contents or synchronization.
class MappedReadbackPolicy {
public:
    using Value = std::uint64_t;
    static constexpr Value MaxBuffer = 512 * 1024;
    static constexpr Value Budget = 16 * 1024 * 1024;
    struct Lease {
        Lease(std::shared_ptr<std::atomic<Value>> used_, Value bytes_)
            : used{std::move(used_)}, bytes{bytes_} {}
        ~Lease() { used->fetch_sub(bytes, std::memory_order_acq_rel); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        std::shared_ptr<std::atomic<Value>> used;
        Value bytes;
    };
    explicit MappedReadbackPolicy(bool enabled_ = false) : enabled{enabled_} {
        if (enabled) preferred.reserve(64);
    }
    bool CanPromote(Value bytes) const {
        return enabled && bytes && bytes <= MaxBuffer && used->load() <= Budget - bytes;
    }
    bool Prefer(Value address, Value bytes) {
        if (!CanPromote(bytes) || address > ~Value{} - bytes) return false;
        const Range range{address, address + bytes};
        if (std::none_of(preferred.begin(), preferred.end(), [&](const Range& p) {
                return p.begin == range.begin && p.end == range.end;
            })) {
            if (preferred.size() == 64) preferred.erase(preferred.begin());
            preferred.push_back(range);
        }
        return true;
    }
    bool Wants(Value address, Value bytes, bool sparse) const {
        if (!enabled || sparse || !bytes || bytes > MaxBuffer || address > ~Value{} - bytes)
            return false;
        return std::any_of(preferred.begin(), preferred.end(), [&](const Range& p) {
            return address <= p.begin && address + bytes >= p.end;
        });
    }
    std::shared_ptr<Lease> Reserve(Value address, Value bytes, bool sparse) {
        if (!Wants(address, bytes, sparse)) return {};
        // Allocate the control block before reserving bytes: allocation failure
        // cannot strand budget. The zero-byte lease owns nothing until the CAS.
        auto lease = std::make_shared<Lease>(used, 0);
        Value previous = used->load(std::memory_order_relaxed);
        while (previous <= Budget - bytes) {
            if (used->compare_exchange_weak(previous, previous + bytes, std::memory_order_acq_rel)) {
                lease->bytes = bytes;
                return lease;
            }
        }
        return {};
    }
    Value Used() const { return used->load(std::memory_order_acquire); }
private:
    struct Range { Value begin, end; };
    bool enabled;
    std::shared_ptr<std::atomic<Value>> used = std::make_shared<std::atomic<Value>>(0);
    std::vector<Range> preferred;
};
} // namespace VideoCommon
