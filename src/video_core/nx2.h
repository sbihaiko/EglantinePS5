// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdlib>
#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Tegra {

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// The running title, for the few places that must behave differently per game.
inline std::atomic<u64> g_nx2_program_id{0};

// Titles whose guest side reads GPU-written buffer data through a path the buffer cache does not
// see, so their buffers must be copied back to guest memory at every fence
// (BufferCache::AsyncBufferDownloads, EDEN_NX2_BUFFER_DOWNLOADS). 0400A3A01EE4C000 is Pokemon
// Pokopia: with the copy-back off its sky is black or flickers. Open work: find what reads it,
// then the list goes.
inline bool Nx2NeedsBufferDownloads() {
    switch (g_nx2_program_id.load(std::memory_order_relaxed)) {
    case 0x0400A3A01EE4C000ULL:
        return true;
    default:
        return false;
    }
}

// Device pages the GPU has written through the buffer cache (shader stores, copies), ever. Title 1
// runs command lists the GPU writes each frame (20-byte entries its culling fills in); read
// without a flush they are stale and the draws behind them (trees) go missing. The bits never
// clear: the dispatch that rewrites a page may still be queued on the split's back when its front
// reads the command list. One bit per 4 KiB of the 36-bit device address space (2 MiB).
inline constexpr u64 Nx2WrittenPageBits = 12;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_gpu_written_pages{};
// The split's back thread's position in its ring (words executed so far), and per page (hashed:
// a collision only makes a stamp later than it is) the position the back was at when it last
// marked the page: whether a command list's pages were written by work the front already saw
// the back finish (dma_pusher.cpp, FlushGpuWrittenCommands).
inline std::atomic<u64> g_nx2_back_position{};
inline constexpr u64 Nx2PageStampBits = 20;
inline std::array<std::atomic<u64>, u64{1} << Nx2PageStampBits> g_nx2_page_stamps{};
[[nodiscard]] inline u64 Nx2PageStamp(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    return g_nx2_page_stamps[page & ((u64{1} << Nx2PageStampBits) - 1)].load(std::memory_order_relaxed);
}

inline void Nx2MarkGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_gpu_written_pages.size() * 64 - 1);
    const u64 first = device_address >> Nx2WrittenPageBits;
    if (const u64 position = g_nx2_back_position.load(std::memory_order_relaxed); position != 0) {
        constexpr u64 stamp_mask = (u64{1} << Nx2PageStampBits) - 1;
        for (u64 page = first; page <= last && page - first < (u64{1} << Nx2PageStampBits); ++page) {
            auto& stamp = g_nx2_page_stamps[page & stamp_mask];
            if (stamp.load(std::memory_order_relaxed) < position) {
                stamp.store(position, std::memory_order_relaxed);
            }
        }
    }
    for (u64 index = first / 64; first <= last && index <= last / 64; ++index) {
        const u64 low = index == first / 64 ? first % 64 : 0;
        const u64 high = index == last / 64 ? last % 64 : 63;
        const u64 bits = (~u64{0} >> (63 - high)) & (~u64{0} << low);
        auto& word = g_nx2_gpu_written_pages[index];
        if ((word.load(std::memory_order_relaxed) & bits) != bits) {
            word.fetch_or(bits, std::memory_order_relaxed);
        }
    }
}

// Device pages the buffer cache has written on the GPU (see Nx2MarkBufferWritten below).
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_buffer_written_pages{};

/// A new emulation session starts: no page has been written by its GPU yet.
inline void Nx2ClearGpuWrittenPages() {
    for (auto& word : g_nx2_gpu_written_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    for (auto& stamp : g_nx2_page_stamps) {
        stamp.store(0, std::memory_order_relaxed);
    }
    for (auto& word : g_nx2_buffer_written_pages) {
        word.store(0, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline bool Nx2IsGpuWritten(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_gpu_written_pages.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

// Device pages the buffer cache has written on the GPU (shader stores, copies), ever: the bitmap
// above without the texture cache's image writes (EglantinePS5 marks those there too, for the
// console's unwritten-page reads). A new image made on stale pages (TextureCache::MarkStale)
// skips its upload unless one of these is set, since a buffer write-back may have landed there.
// (g_nx2_buffer_written_pages is declared above Nx2ClearGpuWrittenPages, which clears it.)

inline void Nx2MarkBufferWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 first = device_address >> Nx2WrittenPageBits;
    const u64 last = std::min<u64>((device_address + size - 1) >> Nx2WrittenPageBits,
                                   g_nx2_buffer_written_pages.size() * 64 - 1);
    for (u64 index = first / 64; first <= last && index <= last / 64; ++index) {
        const u64 low = index == first / 64 ? first % 64 : 0;
        const u64 high = index == last / 64 ? last % 64 : 63;
        const u64 bits = (~u64{0} >> (63 - high)) & (~u64{0} << low);
        auto& word = g_nx2_buffer_written_pages[index];
        if ((word.load(std::memory_order_relaxed) & bits) != bits) {
            word.fetch_or(bits, std::memory_order_relaxed);
        }
    }
}

/// False only when no page of [device_address, device_address + size) has been written by the
/// buffer cache on the GPU. Ranges past the bitmap answer true.
[[nodiscard]] inline bool Nx2MayBeGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return false;
    }
    const u64 first = device_address >> Nx2WrittenPageBits;
    const u64 last = (device_address + size - 1) >> Nx2WrittenPageBits;
    if (last < first || last >= g_nx2_buffer_written_pages.size() * 64) {
        return true;
    }
    for (u64 page = first; page <= last; ++page) {
        if ((g_nx2_buffer_written_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1) {
            return true;
        }
    }
    return false;
}

// The texture cache's GPU writes count as well: render targets, image stores, clears, copies and
// blits (TextureCache::MarkModification). On the console, without a fastmem arena, a guest read
// of a page with no bit set is answered from guest memory without asking the caches (the PS5's
// unwritten-page reads, core/memory.cpp). An image the GPU drew to is only copied back to guest
// memory once a guest read has asked the texture cache for it (GetFlushArea); without the bits
// that never happened, and the guest read the image's memory as it was before the GPU wrote it.
// Pokemon Pokopia draws a 256x3 RGBA32F image and reads it back on the CPU for its sky colours: on
// the console its sky pass got colours of 1e-7 and below and the sky was black.
// EDEN_NX2_IMAGE_WRITTEN_PAGES=0 leaves images out (for comparisons).
[[nodiscard]] inline bool Nx2ImageWrittenPagesEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_IMAGE_WRITTEN_PAGES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// Whether an image's pages are to be marked at this GPU write: once per image, as its pages
/// never move; a sparse image's mapping can change, so again whenever its contents were
/// downloaded or discarded since (no GpuModified flag before this write).
[[nodiscard]] inline bool Nx2ImagePagesNeedMarking(bool marked, bool sparse,
                                                   bool gpu_modified_before) {
    return !marked || (sparse && !gpu_modified_before);
}

/// Marks [device_address, device_address + size) as GPU written unless every page already is
/// (the page stamps are for command lists, which do not live in images).
inline void Nx2MarkImageGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 last = device_address + size - 1;
    if (last < device_address) {
        return;
    }
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last >> Nx2WrittenPageBits;
         ++page) {
        if (!Nx2IsGpuWritten(page << Nx2WrittenPageBits)) {
            Nx2MarkGpuWritten(device_address, size);
            return;
        }
    }
}

template <typename Translate>
[[nodiscard]] bool Nx2HasGpuWrittenPage(GPUVAddr address, size_t size, Translate&& translate) {
    if (size == 0) {
        return false;
    }
    const GPUVAddr last = address + size - 1;
    if (last < address) {
        return true;
    }
    constexpr GPUVAddr mask = (GPUVAddr{1} << Nx2WrittenPageBits) - 1;
    for (GPUVAddr page = address & ~mask;; page += mask + 1) {
        if (const auto device = translate(page); device && Nx2IsGpuWritten(*device)) {
            return true;
        }
        if (page == (last & ~mask)) {
            return false;
        }
    }
}

// Draining the split's back before every command list serialized the two threads: at Mario
// Kart World's title screen the front waited ~69,000 times a second, a third of its time. The
// written-page bits are checked first and only a list on a page the GPU has written waits for
// the back and flushes. A page's first GPU write can still be queued on the back when the front
// reads a list on it (the bit is set as the back runs the write): that list is read once as it
// was. EDEN_NX2_DRAIN_FIRST=1 restores the unconditional drain.
template <typename Drain, typename Translate, typename Flush>
void Nx2SynchronizeGpuWrittenCommands(GPUVAddr address, size_t size, Drain&& drain,
                                     Translate&& translate, Flush&& flush) {
    if (size == 0) return;
    static const bool drain_first = [] {
        const char* value = std::getenv("EDEN_NX2_DRAIN_FIRST");
        return value != nullptr && value[0] == '1';
    }();
    if (drain_first) {
        drain();
        if (Nx2HasGpuWrittenPage(address, size, translate)) flush();
        return;
    }
    if (Nx2HasGpuWrittenPage(address, size, translate)) {
        drain();
        flush();
    }
}

// NVN2 queues release their progress values into the second page of a two-page GPU-accessible
// region and GPU acquires read the same offsets in the first page (the CPU polls the second).
// Something on the console copies one into the other; until that is known, the puller mirrors
// releases that land in a second page. Every queue has such a region, so keep a small table.
inline constexpr size_t Nx2MaxSemaphoreRegions = 32;
inline std::array<std::atomic<u64>, Nx2MaxSemaphoreRegions> g_nx2_semaphore_regions{}; // starts

inline void Nx2AddSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        if (region.load() == start) {
            return;
        }
    }
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = 0;
        if (region.compare_exchange_strong(expected, start)) {
            return;
        }
    }
}

/// The region's memory was destroyed: stop mirroring into it.
inline void Nx2RemoveSemaphoreRegion(u64 start) {
    for (auto& region : g_nx2_semaphore_regions) {
        u64 expected = start;
        region.compare_exchange_strong(expected, 0);
    }
}

/// A new emulation session starts: no queue has a region yet.
inline void Nx2ClearSemaphoreRegions() {
    for (auto& region : g_nx2_semaphore_regions) {
        region.store(0);
    }
}

// Switch 2 sparse regions (nx2:gpu 214, word 7 bit 0x200: title 1's terrain texture arrays and
// layer pools, 170 MB-5.8 GB each, of which the game fills a little) get memory per block when the
// GPU first writes there, not in full. nx2:gpu installs the handler and the span its regions cover;
// the GPU memory manager calls Nx2BackSparseWrite before each write.
// With the bytes being written when known: blocks nobody wrote that only get zeros need no memory.
using Nx2SparseBackingHandler = std::function<void(u64 gpu_address, u64 size, const void* data)>;
inline std::recursive_mutex g_nx2_sparse_backing_mutex;
inline Nx2SparseBackingHandler g_nx2_sparse_backing;
inline std::atomic<u64> g_nx2_sparse_low{~u64{0}};
inline std::atomic<u64> g_nx2_sparse_high{0};

inline void Nx2SetSparseBacking(Nx2SparseBackingHandler handler) {
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    g_nx2_sparse_backing = std::move(handler);
    g_nx2_sparse_low.store(~u64{0});
    g_nx2_sparse_high.store(0);
}

/// Widens the span that writes are checked against (a new lazily backed region).
inline void Nx2AddSparseSpan(u64 gpu_address, u64 size) {
    u64 low = g_nx2_sparse_low.load();
    while (gpu_address < low && !g_nx2_sparse_low.compare_exchange_weak(low, gpu_address)) {
    }
    u64 high = g_nx2_sparse_high.load();
    while (gpu_address + size > high &&
           !g_nx2_sparse_high.compare_exchange_weak(high, gpu_address + size)) {
    }
}

inline void Nx2BackSparseWrite(u64 gpu_address, u64 size, const void* data = nullptr) {
    if (gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_backing_mutex};
    if (g_nx2_sparse_backing) {
        g_nx2_sparse_backing(gpu_address, size, data);
    }
}

// Which blocks of each sparse region have been written (got memory), for the texture cache: an
// image over blocks nobody wrote is all zeros and needs no device memory. Its own lock, taken last:
// the texture cache asks with its lock held, and nx2:gpu updates it holding only its region mutex.
struct Nx2SparseRegionState {
    u64 size;
    u64 block_size;
    std::vector<bool> written;
    /// Once the game maps its own memory into the region (nx2:gpu 216): which tiles it maps.
    /// Only those hold data then (writes elsewhere are dropped, as on the console).
    std::vector<bool> mapped_pages;
};

/// Coverage granularity of accepted guest GPU mappings, independent of host sparse blocks.
inline constexpr u64 Nx2SparseMappingPageSize = 4ULL << 10;
inline std::mutex g_nx2_sparse_state_mutex;
inline std::map<u64, Nx2SparseRegionState> g_nx2_sparse_state;

inline void Nx2SparseRegionCreated(u64 gpu_address, u64 size, u64 block_size) {
    if (size == 0 || block_size == 0 || size > (std::numeric_limits<u64>::max)() - gpu_address) {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state[gpu_address] = {
        size, block_size, std::vector<bool>(size / block_size + (size % block_size != 0))};
}

inline void Nx2SparseRegionDestroyed(u64 gpu_address) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.erase(gpu_address);
}

/// A new emulation session: the previous game's regions are gone (nvdrv's Module clears them).
inline void Nx2ClearSparseRegions() {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    g_nx2_sparse_state.clear();
}

inline void Nx2SparseBlockWritten(u64 region_address, u64 block) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    if (const auto it = g_nx2_sparse_state.find(region_address);
        it != g_nx2_sparse_state.end() && block < it->second.written.size()) {
        it->second.written[block] = true;
    }
}

/// The game maps its own memory over [offset, +size) of the region at region_address (mapped), or
/// unmaps it there.
inline void Nx2SparseTilesMapped(u64 region_address, u64 offset, u64 size, bool mapped) {
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    const auto it = g_nx2_sparse_state.find(region_address);
    if (it == g_nx2_sparse_state.end() || size == 0) {
        return;
    }
    auto& region = it->second;
    if (offset >= region.size || size > region.size - offset) {
        return;
    }
    if (region.mapped_pages.empty()) {
        region.mapped_pages.resize(region.size / Nx2SparseMappingPageSize +
                                   (region.size % Nx2SparseMappingPageSize != 0));
    }
    const u64 last = (offset + size - 1) / Nx2SparseMappingPageSize;
    for (u64 tile = offset / Nx2SparseMappingPageSize; tile <= last; ++tile) {
        region.mapped_pages[tile] = mapped;
    }
}

/// The parts of [gpu_address, +size), which lies in one sparse region, that can hold data: the
/// tiles the game maps there if it maps any, otherwise the blocks written. As (offset from
/// gpu_address, size) runs.
inline std::vector<std::pair<u64, u64>> Nx2SparseDataRanges(u64 gpu_address, u64 size) {
    std::vector<std::pair<u64, u64>> ranges;
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return ranges;
    }
    --it;
    const auto& [start, region] = *it;
    if (size == 0 || gpu_address - start >= region.size ||
        size > region.size - (gpu_address - start)) {
        return ranges;
    }
    const bool by_tile = !region.mapped_pages.empty();
    const u64 unit = by_tile ? Nx2SparseMappingPageSize : region.block_size;
    const std::vector<bool>& present = by_tile ? region.mapped_pages : region.written;
    const u64 begin = gpu_address - start;
    const u64 end = begin + size;
    for (u64 index = begin / unit; index * unit < end && index < present.size(); ++index) {
        if (!present[index]) {
            continue;
        }
        const u64 run_begin = (std::max)(index * unit, begin) - begin;
        const u64 run_end = index * unit + (std::min)(unit, end - index * unit) - begin;
        if (!ranges.empty() && ranges.back().first + ranges.back().second == run_begin) {
            ranges.back().second += run_end - run_begin;
        } else {
            ranges.emplace_back(run_begin, run_end - run_begin);
        }
    }
    return ranges;
}

/// True when [gpu_address, +size) lies in one sparse region.
inline bool Nx2InSparseRegion(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    return gpu_address + size <= it->first + it->second.size;
}

/// The GPU renders into [gpu_address, +size): count its blocks as written, so images made there
/// later get memory (their contents live in the host image, not in guest memory).
inline void Nx2SparseMarkWritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    for (auto& [start, region] : g_nx2_sparse_state) {
        const u64 end = start + region.size;
        if (gpu_address >= end || gpu_address + size <= start) {
            continue;
        }
        const u64 first = ((std::max)(gpu_address, start) - start) / region.block_size;
        const u64 last = ((std::min)(gpu_address + size, end) - 1 - start) / region.block_size;
        for (u64 block = first; block <= last; ++block) {
            region.written[block] = true;
        }
    }
}

/// True when [gpu_address, +size) lies in one sparse region and none of it has been written.
inline bool Nx2SparseUnwritten(u64 gpu_address, u64 size) {
    if (size == 0 || gpu_address + size <= g_nx2_sparse_low.load(std::memory_order_relaxed) ||
        gpu_address >= g_nx2_sparse_high.load(std::memory_order_relaxed)) [[likely]] {
        return false;
    }
    std::scoped_lock lk{g_nx2_sparse_state_mutex};
    auto it = g_nx2_sparse_state.upper_bound(gpu_address);
    if (it == g_nx2_sparse_state.begin()) {
        return false;
    }
    --it;
    const auto& [start, region] = *it;
    if (gpu_address + size > start + region.size) {
        return false;
    }
    const u64 last = (gpu_address + size - 1 - start) / region.block_size;
    for (u64 block = (gpu_address - start) / region.block_size; block <= last; ++block) {
        if (region.written[block]) {
            return false;
        }
    }
    return true;
}

/// Channels suspended on a semaphore acquire. A console channel polls and sees every release;
/// Eden writes releases in bursts when fences signal and resumes waiters only between command
/// lists, so an acquire-equal could miss its value. Writes latch the waiters they satisfy.
class Nx2SemaphoreWaiters {
public:
    // Puller GpuSemaphoreOperation values.
    static constexpr u32 AcquireEqual = 0x1;
    static constexpr u32 AcquireGequal = 0x4;
    static constexpr u32 AcquireMask = 0x8;

    static Nx2SemaphoreWaiters& Get() {
        static Nx2SemaphoreWaiters waiters;
        return waiters;
    }

    void Add(const void* owner, u64 device_address, u32 value, u32 operation) {
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        waiters.push_back({owner, device_address, value, operation, false});
        count.store(waiters.size(), std::memory_order_release);
    }

    void Remove(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        count.store(waiters.size(), std::memory_order_release);
    }

    [[nodiscard]] bool Latched(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::scoped_lock lk{mutex};
        return std::ranges::any_of(waiters,
                                   [owner](const Waiter& w) { return w.owner == owner && w.latched; });
    }

    /// A GPU write of `value` to the u32 at `device_address`.
    void OnWrite(u64 device_address, u32 value) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        for (auto& w : waiters) {
            if (w.address == device_address && !w.latched && Matches(w, value)) {
                w.latched = true;
            }
        }
    }

private:
    struct Waiter {
        const void* owner;
        u64 address;
        u32 value;
        u32 operation;
        bool latched;
    };

    static bool Matches(const Waiter& w, u32 current) {
        switch (w.operation) {
        case AcquireEqual:
            return current == w.value;
        case AcquireGequal:
            return current >= w.value;
        case AcquireMask:
            return (current & w.value) != 0;
        default:
            return false;
        }
    }

    std::mutex mutex;
    std::vector<Waiter> waiters;
    std::atomic<size_t> count{};
};

} // namespace Tegra
