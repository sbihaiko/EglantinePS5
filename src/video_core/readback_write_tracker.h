// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>
#include "video_core/readback_range_tracker.h"

namespace VideoCommon {
// Diagnostic writer coverage only. Does not retain Vulkan allocations or authorize
// mapped reads. Sparse aliases and external writers need explicit invalidation.
template <class Handle>
class ReadbackWriteTracker {
public:
    using Value = ReadbackRangeTracker::Value;
    struct State {
        State(Value identity_, Value size, std::shared_ptr<std::atomic<Value>> epoch_)
            : identity{identity_}, ranges{identity_, size}, epoch{std::move(epoch_)},
              observed_epoch{epoch->load(std::memory_order_acquire)} {}
        void SyncEpoch() {
            const auto current = epoch->load(std::memory_order_acquire);
            if (observed_epoch != current) {
                ranges.InvalidateAll();
                observed_epoch = current;
            }
        }
        Value identity;
        ReadbackRangeTracker ranges;
        std::shared_ptr<std::atomic<Value>> epoch;
        Value observed_epoch;
        std::size_t pending{};
        bool external_writer{};
        std::optional<ReadbackRangeTracker::Ticket> Lookup(Value offset, Value size) const {
            const auto current = epoch->load(std::memory_order_acquire);
            return pending || current == 0 || observed_epoch != current
                ? std::nullopt : ranges.Lookup(offset, size);
        }
    };
    explicit ReadbackWriteTracker(bool enabled = false) : enabled{enabled} {
        if (enabled) writes.reserve(MaxWrites);
    }
    std::shared_ptr<State> Register(Handle handle, Value size, bool sparse) {
        if (!enabled || sparse || size == 0 || size > 512 * 1024) return {};
        Prune();
        if (states.size() >= 256 || next_identity == 0) return {};
        auto state = std::make_shared<State>(next_identity++, size, epoch);
        states[handle] = state;
        return state;
    }
    bool Enabled() const { return enabled; }
    void InvalidateAll() {
        // Mapping notifications may arrive outside the GPU/cache owner. They only
        // publish an epoch; range vectors are changed by the owner on its next use.
        Value current = epoch->load(std::memory_order_relaxed);
        while (current != 0 && !epoch->compare_exchange_weak(
                   current, current + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
        // Zero is absorbing on epoch exhaustion, so no old identity can become valid again.
    }
    void Prune() {
        std::erase_if(states, [](const auto& entry) { return entry.second.expired(); });
    }
    void BeginBatch() {
        if (!enabled) return;
        AbortBatch();
        active = true;
        overflow = false;
    }
    void NoteShaderWrite(Handle handle, Value offset, Value size) {
        if (!enabled) return;
        auto state = Find(handle);
        if (!state) return;
        state->ranges.Invalidate(offset, size);
        if (!active) state->external_writer = true;
        if (!active || overflow) return;
        if (writes.size() == MaxWrites) {
            AbortBatch();
            overflow = true;
            InvalidateAll();
            enabled = false; // Coverage exhausted: never publish a partial batch.
            return;
        }
        ++state->pending;
        writes.push_back({std::move(state), offset, size});
    }
    // Invoke immediately after enqueueing the draw/dispatch, before any Flush.
    void CompleteBatch(Value tick) {
        if (!enabled) return;
        for (auto& write : writes) {
            write.state->SyncEpoch();
            write.state->ranges.RecordGpuWrite(write.offset, write.size, tick);
            --write.state->pending;
        }
        writes.clear();
        active = false;
    }
    void AbortBatch() {
        for (auto& write : writes) {
            write.state->ranges.Invalidate(write.offset, write.size);
            --write.state->pending;
        }
        writes.clear();
        active = false;
    }
    void TransferWrite(Handle handle, Value offset, Value size, Value tick, bool reordered) {
        if (!enabled) return;
        if (auto state = Find(handle)) {
            if (reordered) state->ranges.InvalidateAll();
            else state->ranges.RecordGpuWrite(offset, size, tick);
        }
    }
    void Invalidate(Handle handle) {
        if (!enabled) return;
        if (auto state = Find(handle)) state->ranges.InvalidateAll();
    }
private:
    std::shared_ptr<State> Find(Handle handle) {
        const auto found = states.find(handle);
        auto state = found == states.end() ? nullptr : found->second.lock();
        if (state) state->SyncEpoch();
        return state;
    }
    struct Write { std::shared_ptr<State> state; Value offset, size; };
    static constexpr std::size_t MaxWrites = 1024;
    bool enabled{}, active{}, overflow{};
    Value next_identity{1};
    std::shared_ptr<std::atomic<Value>> epoch = std::make_shared<std::atomic<Value>>(1);
    std::unordered_map<Handle, std::weak_ptr<State>> states;
    std::vector<Write> writes;
};
} // namespace VideoCommon
