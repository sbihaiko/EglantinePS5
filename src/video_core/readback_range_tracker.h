// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace VideoCommon {

// Single GPU-owner bookkeeping for one allocation lifetime and one submission timeline.
// This is not a lock or a memory barrier. Callers must exclude overlapping writers while
// validating and consuming a ticket, retain the allocation, and perform host visibility
// barriers. Every GPU writer must be recorded at command recording, not descriptor binding.
// Untracked writes, aliases and remaps must invalidate coverage. Allocation IDs must never
// be reused while tickets exist. There is deliberately no implicit "initially clean" state.
class ReadbackRangeTracker {
public:
    using Value = std::uint64_t;
    struct Ticket {
        Value allocation{}, epoch{}, offset{}, size{}, version{}, producer{};
    };

    explicit ReadbackRangeTracker(Value allocation_id, Value allocation_size,
                                  std::size_t segment_limit = 64)
        : allocation{allocation_id}, size{allocation_size},
          limit{std::min<std::size_t>(segment_limit, 1024)} {
        segments.reserve(limit + 2);
        scratch.reserve(limit + 2);
    }

    ReadbackRangeTracker(const ReadbackRangeTracker&) = delete;
    ReadbackRangeTracker& operator=(const ReadbackRangeTracker&) = delete;

    ReadbackRangeTracker(ReadbackRangeTracker&& other) noexcept
        : allocation{std::exchange(other.allocation, 0)}, size{other.size},
          epoch{other.epoch}, version{other.version}, last_producer{other.last_producer},
          limit{other.limit}, segments{std::move(other.segments)},
          scratch{std::move(other.scratch)} {}

    ReadbackRangeTracker& operator=(ReadbackRangeTracker&& other) noexcept {
        if (this != &other) {
            allocation = std::exchange(other.allocation, 0);
            size = other.size;
            epoch = other.epoch;
            version = other.version;
            last_producer = other.last_producer;
            limit = other.limit;
            segments = std::move(other.segments);
            scratch = std::move(other.scratch);
        }
        return *this;
    }

    // Record the submission containing the actual writer. Zero and timeline regression
    // are rejected. Submission completion is queried separately by the caller.
    bool RecordGpuWrite(Value offset, Value length, Value producer) {
        if (!ValidRange(offset, length) || producer == 0 || producer < last_producer) {
            InvalidateAll();
            return false;
        }
        last_producer = producer;
        if (!AdvanceVersion()) {
            return false;
        }
        return Replace(offset, length, producer);
    }

    void Invalidate(Value offset, Value length) {
        if (!ValidRange(offset, length)) {
            InvalidateAll();
            return;
        }
        if (AdvanceVersion()) {
            Replace(offset, length, 0);
        }
    }

    void InvalidateAll() {
        segments.clear();
        if (epoch == std::numeric_limits<Value>::max()) {
            allocation = 0; // Permanently fail closed on identity exhaustion.
        } else {
            ++epoch;
        }
    }

    [[nodiscard]] std::optional<Ticket> Lookup(Value offset, Value length) const {
        if (!ValidRange(offset, length)) {
            return std::nullopt;
        }
        const Value end = offset + length;
        Value covered = offset, producer = 0, newest = 0;
        for (const auto& segment : segments) {
            if (segment.end <= covered) {
                continue;
            }
            if (segment.begin > covered || segment.producer == 0) {
                return std::nullopt;
            }
            producer = std::max(producer, segment.producer);
            newest = std::max(newest, segment.version);
            covered = std::min(end, segment.end);
            if (covered == end) {
                return Ticket{allocation, epoch, offset, length, newest, producer};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] bool IsCurrent(const Ticket& ticket) const {
        if (ticket.allocation != allocation || ticket.epoch != epoch) {
            return false;
        }
        const auto current = Lookup(ticket.offset, ticket.size);
        return current && current->version == ticket.version &&
               current->producer == ticket.producer;
    }

    [[nodiscard]] std::size_t SegmentCount() const { return segments.size(); }

private:
    struct Segment {
        Value begin, end, producer, version;
    };

    bool ValidRange(Value offset, Value length) const {
        return allocation != 0 && length != 0 && offset < size && length <= size - offset;
    }

    bool AdvanceVersion() {
        if (version == std::numeric_limits<Value>::max()) {
            InvalidateAll();
            allocation = 0;
            return false;
        }
        ++version;
        return true;
    }

    bool Replace(Value offset, Value length, Value producer) {
        const Value end = offset + length;
        // At most two extra splits; storage is reserved once, not per GPU write.
        auto& next = scratch;
        next.clear();
        for (const auto& segment : segments) {
            if (segment.end <= offset || segment.begin >= end) {
                next.push_back(segment);
                continue;
            }
            if (segment.begin < offset) {
                next.push_back({segment.begin, offset, segment.producer, segment.version});
            }
            if (segment.end > end) {
                next.push_back({end, segment.end, segment.producer, segment.version});
            }
        }
        // Unknown coverage is represented by a gap, never a completion ticket.
        if (producer != 0) {
            next.push_back({offset, end, producer, version});
        }
        if (next.size() > limit) {
            InvalidateAll(); // Bounded metadata; callers retain the staging fallback.
            return false;
        }
        std::sort(next.begin(), next.end(), [](const Segment& a, const Segment& b) {
            return a.begin < b.begin;
        });
        segments.swap(next);
        return true;
    }

    Value allocation{}, size{}, epoch{1}, version{}, last_producer{};
    std::size_t limit{};
    std::vector<Segment> segments;
    std::vector<Segment> scratch;
};

} // namespace VideoCommon
