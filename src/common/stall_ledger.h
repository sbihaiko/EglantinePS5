// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

// EDEN_NX2_STALL_REPORT=<ms>: where the time of a long frame went. The sites below add what they
// spent to one ledger for the process (relaxed counters: time, count, bytes, and the longest
// piece); the frontend (headless/graphics.cpp) reads it at every presented frame and prints one
// EDEN_STALL line for a frame longer than <ms>, with what the ledger gained during that frame.
// Unset or 0: off, each site costs one check of a cached flag, and nothing is printed.
// Header only, so the core and the frontend share it.
namespace Common::StallLedger {

enum class Category : std::size_t {
    CramUnpack,    ///< A fault unpacks a packed slot of the guest's memory (the faulting thread)
    CramPack,      ///< A slot packed (the scanner thread, or a thread in RecoverMemory)
    CramRecover,   ///< RecoverMemory: the driver or the heap ran out (the thread that asked)
    CramWait,      ///< A written slot waited for a block (WaitForBlock: the faulting thread)
    BufferCreate,  ///< Buffer cache: a buffer made, overlaps merged into it (GPU thread)
    BufferUpload,  ///< Buffer cache: guest memory uploaded to a buffer (GPU thread)
    StagingCreate, ///< A staging buffer made (the thread that asked, mostly the GPU thread)
    PipelineBuild, ///< A pipeline built on the GPU thread
    PipelineWait,  ///< The GPU thread waited for a pipeline a worker translates
    ListWait,      ///< Split GPU thread: the front waited for the back, a GPU-written list
    SplitWait,     ///< Split GPU thread: the front waited for the back otherwise (drains, ring full)
    TextureGc,     ///< Texture cache: the garbage collector (GPU thread)
    TextureUpload, ///< Texture cache: an image's contents uploaded (GPU thread)
    FenceWait,     ///< A wait for a GPU tick that was not reached yet (any thread)
    AudioLate,     ///< Audio: a submit came after the port had run dry (the audio output thread)
    AudioDry,      ///< Audio: a block padded with the held frame, the queue empty (same thread)
    Count,
};

inline constexpr std::size_t CategoryCount = static_cast<std::size_t>(Category::Count);

struct Counter {
    std::atomic<std::uint64_t> ns{0};
    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> max_ns{0}; ///< Taken (and cleared) by the reporter
};

inline std::array<Counter, CategoryCount> counters{};

using Clock = std::chrono::steady_clock;

/// The time the process started (its static initialization), for the reports' t=.
inline const Clock::time_point process_start = Clock::now();

/// The frame time that counts as a stall, in ms; 0 when off.
[[nodiscard]] inline std::uint64_t ThresholdMs() {
    static const std::uint64_t ms = [] {
        const char* const value = std::getenv("EDEN_NX2_STALL_REPORT");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    return ms;
}

[[nodiscard]] inline bool Enabled() {
    static const bool enabled = ThresholdMs() != 0;
    return enabled;
}

[[nodiscard]] inline double SecondsSinceStart(Clock::time_point when = Clock::now()) noexcept {
    return std::chrono::duration<double>(when - process_start).count();
}

inline void Add(Category category, std::uint64_t ns, std::uint64_t count, std::uint64_t bytes) noexcept {
    Counter& counter = counters[static_cast<std::size_t>(category)];
    counter.ns.fetch_add(ns, std::memory_order_relaxed);
    counter.count.fetch_add(count, std::memory_order_relaxed);
    if (bytes != 0) {
        counter.bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

/// Keeps the longest piece of `category` since the reporter last took it.
inline void NoteMax(Category category, std::uint64_t ns) noexcept {
    std::atomic<std::uint64_t>& max = counters[static_cast<std::size_t>(category)].max_ns;
    std::uint64_t seen = max.load(std::memory_order_relaxed);
    while (seen < ns && !max.compare_exchange_weak(seen, ns, std::memory_order_relaxed)) {
    }
}

/// The start of a timed piece: now when on, the zero time point when off.
[[nodiscard]] inline Clock::time_point Begin() {
    return Enabled() ? Clock::now() : Clock::time_point{};
}

/// Adds the time since `begin` (from Begin) and `bytes` to `category`; nothing when off.
inline void End(Category category, Clock::time_point begin, std::uint64_t bytes = 0) noexcept {
    if (begin == Clock::time_point{}) {
        return;
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
    Add(category, static_cast<std::uint64_t>(ns > 0 ? ns : 0), 1, bytes);
}

/// Times its own scope into `category`.
class Scope {
public:
    explicit Scope(Category category_, std::uint64_t bytes_ = 0) : category{category_}, bytes{bytes_}, begin{Begin()} {}
    ~Scope() {
        End(category, begin, bytes);
    }
    void SetBytes(std::uint64_t value) noexcept {
        bytes = value;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Category category;
    std::uint64_t bytes;
    Clock::time_point begin;
};

/// Every counter now. The reporter subtracts the previous one, so nothing is cleared under the
/// threads that add.
struct Totals {
    std::array<std::uint64_t, CategoryCount> ns{};
    std::array<std::uint64_t, CategoryCount> count{};
    std::array<std::uint64_t, CategoryCount> bytes{};
};

[[nodiscard]] inline Totals Read() noexcept {
    Totals totals;
    for (std::size_t i = 0; i < CategoryCount; ++i) {
        totals.ns[i] = counters[i].ns.load(std::memory_order_relaxed);
        totals.count[i] = counters[i].count.load(std::memory_order_relaxed);
        totals.bytes[i] = counters[i].bytes.load(std::memory_order_relaxed);
    }
    return totals;
}

[[nodiscard]] inline std::uint64_t TakeMax(Category category) noexcept {
    return counters[static_cast<std::size_t>(category)].max_ns.exchange(0, std::memory_order_relaxed);
}

/// One audio gap waiting to be printed (EDEN_AUDIO_GAP). The audio thread does not print: a
/// print that meets the log flush waits for the storage, and the port would run dry again. It
/// leaves the gap here and the frontend's frame report prints it. Bits 0-21: the gap in ms;
/// bit 22: 1 for a dry queue, 0 for a late submit; bits 23-63: ms since the process started.
inline std::atomic<std::uint64_t> audio_gap_post{0};

inline void PostAudioGap(Clock::time_point when, std::uint64_t gap_ms, bool dry) noexcept {
    const auto at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(when - process_start).count();
    const std::uint64_t gap = gap_ms < (1ULL << 22) ? gap_ms : (1ULL << 22) - 1;
    audio_gap_post.store((static_cast<std::uint64_t>(at_ms > 0 ? at_ms : 0) << 23) | (dry ? 1ULL << 22 : 0) | gap,
                         std::memory_order_relaxed);
}

struct AudioGap {
    double seconds;
    std::uint64_t gap_ms;
    bool dry;
};

/// The posted gap, if any, taken.
[[nodiscard]] inline bool TakeAudioGap(AudioGap& gap) noexcept {
    const std::uint64_t value = audio_gap_post.exchange(0, std::memory_order_relaxed);
    if (value == 0) {
        return false;
    }
    gap = AudioGap{static_cast<double>(value >> 23) / 1000.0, value & ((1ULL << 22) - 1), ((value >> 22) & 1) != 0};
    return true;
}

} // namespace Common::StallLedger
