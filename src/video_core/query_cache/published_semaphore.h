// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include "common/spin_recursive_mutex.h"
#include <optional>

namespace VideoCommon {

// Bounded CPU-visible copies of completed 32-bit semaphore releases. A ticket
// identifies one address lifetime and one queued release; queuing newer work
// does not hide the last completed value. Invalidation revokes every old ticket.
// Callers still establish semaphore semantics, mapping identity and absence of
// competing buffer/texture writes before using a published value.
template <std::size_t Capacity = 256>
class PublishedSemaphoreStore {
    static_assert(Capacity > 0);
public:
    struct Ticket {
        std::uint64_t address{}, epoch{}, sequence{};
        const void* pointer{};
        explicit operator bool() const { return epoch != 0; }
    };
    Ticket Begin(std::uint64_t address, const void* pointer) {
        if (!pointer || (address & 3) || address > std::numeric_limits<std::uint64_t>::max()-3)
            return {};
        std::lock_guard lock{mutex};
        // Never recycle ticket identities, including after counter exhaustion.
        if (serial == std::numeric_limits<std::uint64_t>::max()) return {};
        auto& entry = entries[Index(address)];
        const auto sequence = ++serial;
        if (!entry.epoch || entry.address != address || entry.pointer != pointer)
            entry = Entry{address, sequence, 0, pointer, 0, false};
        return {address, entry.epoch, sequence, pointer};
    }
    bool Complete(Ticket ticket, std::uint32_t value) {
        if (!ticket) return false;
        std::lock_guard lock{mutex};
        auto& entry = entries[Index(ticket.address)];
        if (entry.epoch != ticket.epoch || entry.address != ticket.address ||
            entry.pointer != ticket.pointer || ticket.sequence <= entry.completed_sequence)
            return false;
        entry.value = value;
        entry.completed_sequence = ticket.sequence;
        entry.valid = true;
        return true;
    }
    std::optional<std::uint32_t> Read(std::uint64_t address, const void* pointer) {
        std::lock_guard lock{mutex};
        const auto& entry = entries[Index(address)];
        if (!entry.valid || entry.address != address || entry.pointer != pointer) return {};
        return entry.value;
    }
    void Invalidate(std::uint64_t address, std::uint64_t size) {
        if (!size) return;
        std::lock_guard lock{mutex};
        const auto invalidate = [&](Entry& entry) {
            if (!entry.epoch) return;
            // Subtractions avoid overflowing either half-open range end.
            if (entry.address >= address ? entry.address-address < size
                                         : address-entry.address < 4)
                entry = {};
        };
        if (size < Capacity * 4) {
            const auto words = (size + (address & 3) + 3) / 4;
            auto word = address & ~std::uint64_t{3};
            for (std::uint64_t i = 0; i < words; ++i) {
                invalidate(entries[Index(word)]);
                if (word > std::numeric_limits<std::uint64_t>::max()-4) break;
                word += 4;
            }
        } else {
            for (auto& entry : entries) invalidate(entry);
        }
    }
    void Clear() {
        std::lock_guard lock{mutex};
        entries = {};
        // Keep serial: callbacks from before Clear must not match a new lifetime.
    }
private:
    struct Entry {
        std::uint64_t address{}, epoch{}, completed_sequence{};
        const void* pointer{};
        std::uint32_t value{};
        bool valid{};
    };
    static std::size_t Index(std::uint64_t address) {
        // Mix page bits too: low-bit multiplication maps all page-aligned
        // semaphore words to slot zero in a power-of-two table.
        auto key = address >> 2;
        key ^= key >> 30;
        key *= 0xbf58476d1ce4e5b9ULL;
        key ^= key >> 27;
        key *= 0x94d049bb133111ebULL;
        key ^= key >> 31;
        return key % Capacity;
    }
    // Taken per guest write and per inline upload from several threads for a few instructions:
    // a sleeping mutex cost the split's back thread a system call whenever a guest core held it.
    Common::SpinRecursiveMutex mutex;
    std::array<Entry, Capacity> entries{};
    std::uint64_t serial{};
};
}
