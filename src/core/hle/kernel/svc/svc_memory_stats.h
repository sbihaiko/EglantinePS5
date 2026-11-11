// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Counts of the guest's memory system calls (diagnostic): how often a game changes its heap or
// maps memory over a session, and how many bytes those calls name. Reported by the frontend
// (EDEN_SVC_MEMORY in stderr.log).

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>

#include "common/common_types.h"

namespace Kernel::Svc::MemoryStats {

enum Call : int {
    SetHeapSize, MapPhysicalMemory, UnmapPhysicalMemory, MapMemory, UnmapMemory,
    SetMemoryPermission, SetMemoryAttribute, CreateTransferMemory, MapSharedMemory,
    CreateCodeMemory, ControlCodeMemory, MapProcessMemory, Count
};

inline constexpr std::array<const char*, Count> Names{
    "SetHeapSize", "MapPhysicalMemory", "UnmapPhysicalMemory", "MapMemory", "UnmapMemory",
    "SetMemoryPermission", "SetMemoryAttribute", "CreateTransferMemory", "MapSharedMemory",
    "CreateCodeMemory", "ControlCodeMemory", "MapProcessMemory"};

struct Counter {
    std::atomic<u64> calls{}, bytes{}, failed{};
};

inline std::array<Counter, Count>& Counters() {
    static std::array<Counter, Count> counters;
    return counters;
}

/// The heap size the game asked for last (SetHeapSize), and its largest.
inline std::atomic<u64>& LastHeapSize() {
    static std::atomic<u64> value{};
    return value;
}
inline std::atomic<u64>& LargestHeapSize() {
    static std::atomic<u64> value{};
    return value;
}

inline void Note(Call call, u64 bytes, bool ok) {
    auto& counter = Counters()[call];
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    counter.bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (!ok) {
        counter.failed.fetch_add(1, std::memory_order_relaxed);
    }
}

/// One line: every call kind used so far, with calls, failures and MiB named (cumulative).
inline void Report(std::FILE* out) {
    std::fprintf(out, "EDEN_SVC_MEMORY heap_last=%lluMiB heap_max=%lluMiB",
                 static_cast<unsigned long long>(LastHeapSize().load() >> 20),
                 static_cast<unsigned long long>(LargestHeapSize().load() >> 20));
    for (int i = 0; i < Count; ++i) {
        const auto& counter = Counters()[i];
        const u64 calls = counter.calls.load(std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        std::fprintf(out, " %s=%llu/%lluMiB", Names[i], static_cast<unsigned long long>(calls),
                     static_cast<unsigned long long>(counter.bytes.load(std::memory_order_relaxed) >> 20));
        if (const u64 failed = counter.failed.load(std::memory_order_relaxed)) {
            std::fprintf(out, "(%llu failed)", static_cast<unsigned long long>(failed));
        }
    }
    std::fprintf(out, "\n");
}

} // namespace Kernel::Svc::MemoryStats
