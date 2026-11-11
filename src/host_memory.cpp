// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/host_memory.h"
#include "fastmem.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef PS5_NATIVE
extern "C" {
std::int64_t sceKernelGetDirectMemorySize();
std::int32_t sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
std::int32_t sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
std::int32_t sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
std::int32_t sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
std::int32_t sceKernelEnableDmemAliasing();
int sceKernelGetPageTableStats(int*, int*, int*, int*);
}
#endif

// Self-test probe: a load that may fault, and the fallback a fault is sent to.
extern "C" {
std::uint64_t eden_fastmem_probe_load(const void* address, std::uint64_t* rsp);
void eden_fastmem_probe_load_fault();
void eden_fastmem_probe_load_resume();
void eden_fastmem_probe_fallback();
}
asm(R"(
    .text
    .p2align 4
    .globl eden_fastmem_probe_load, eden_fastmem_probe_load_fault, eden_fastmem_probe_load_resume
eden_fastmem_probe_load:
    movq %rsp, (%rsi)
eden_fastmem_probe_load_fault:
    movq (%rdi), %rax
eden_fastmem_probe_load_resume:
    ret
    .p2align 4
    .globl eden_fastmem_probe_fallback
eden_fastmem_probe_fallback:
    movabsq $0x5a17ed5a17ed5a17, %rax
    ret
)");

namespace Common {
void* AllocateMemoryPages(size_t size) noexcept;
void FreeMemoryPages(void* base, size_t size) noexcept;
// The guest's memory (memory_pages.cpp, guest_memory_service.inc): backed where the guest uses it, and given back
// when a slot holds only zeroes again. With a fastmem window the block must be one piece (AllocateGuestMemory).
void* AllocateGuestMemory(size_t size) noexcept;
void* AllocateGuestMemoryWhereUsed(size_t size) noexcept;
void ClearGuestMemory(std::uint8_t* at, std::uint64_t offset, std::size_t bytes, int fill) noexcept;
void FreeGuestMemory(void* base, size_t size) noexcept;
// The 64-bit window's aliases of the guest's slots (guest_memory_service.inc).
bool AliasGuestSlot(std::uintptr_t backing, std::uintptr_t window_at, bool writable, bool write, bool* promote) noexcept;
bool AliasGuestPage(std::uintptr_t backing, std::uintptr_t at, std::uintptr_t region_at, bool writable, bool write,
                    bool* promote) noexcept;
bool DropGuestAlias(std::uintptr_t backing, std::uintptr_t window_at) noexcept;
void GuestAliasCounts(std::uint64_t* made, std::uint64_t* dropped, std::uint64_t* refused) noexcept;
#ifdef PS5_NATIVE
std::int64_t DirectMemoryStart(const void* pointer) noexcept;
int ReserveOver(void* at, std::size_t size, std::size_t alignment) noexcept;
#endif
} // namespace Common

namespace {
constexpr std::size_t GuestPage = 0x1000;
constexpr std::size_t GuestPages = std::size_t{1} << 20; // the A32 address space
constexpr std::size_t Granule = 0x4000; // PS5 kernel page; host checks use the same groups
constexpr std::size_t PagesPerGranule = Granule / GuestPage;
constexpr std::size_t Chunks = GuestPages / PagesPerGranule;
constexpr std::size_t WindowBytes = GuestPages * GuestPage;
// One byte per guest page below the window: the JIT reads it at [r13 + page - MapBytes]
// (headless/checked-fastmem.cmake). A guard granule after the window stays reserved.
constexpr std::size_t MapBytes = GuestPages;
constexpr std::size_t ReserveBytes = MapBytes + WindowBytes + Granule;
constexpr std::uint8_t Readable = 1, Writable = 2; // Eden's requested page access
constexpr std::uint8_t ReadBlocked = 1, WriteBlocked = 2, Blocked = 3; // map byte bits
constexpr std::uint64_t ProbeMarker = 0x5a17ed5a17ed5a17ull;

std::atomic<bool> window_requested{false};
struct Counters {
    std::atomic<std::uint64_t> window, mapped_pages, aliased_chunks;
    std::atomic<std::uint64_t> map_calls, unmap_calls, protect_calls, kernel_calls, kernel_ns, failures;
} counters;
std::atomic<const std::uint8_t*> current_map{nullptr};

std::uint8_t Access(Common::MemoryPermission perms) {
    return (True(perms & Common::MemoryPermission::Read) ? Readable : 0) |
           (True(perms & Common::MemoryPermission::Write) ? Writable : 0);
}

[[noreturn]] void Fatal(const char* operation, std::size_t chunk, std::size_t count) {
    // A stale alias would expose freed memory to JIT code.
    std::printf("EDEN_FASTMEM_FATAL op=%s chunk=%zu count=%zu errno=%d\n", operation, chunk, count, errno);
    std::fflush(stdout);
    std::abort();
}

// Fault probe state; only the window self-test installs ProbeHandler.
struct Probe {
    std::uint64_t rsp;
    int signal;
    unsigned hits;
} probe;
void ProbeHandler(int signal, siginfo_t*, void* context) {
    auto& rip = Eden::Fastmem::ContextRip(context);
    if (rip != reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_load_fault)) {
        std::signal(signal, SIG_DFL); // Not a probe: repeat the fault with the default action.
        return;
    }
    // The same fake call dynarmic's handler makes for a fastmem fault.
    auto& rsp = Eden::Fastmem::ContextRsp(context);
    probe.rsp = rsp;
    probe.signal = signal;
    ++probe.hits;
    rsp -= sizeof(std::uint64_t);
    *reinterpret_cast<std::uint64_t*>(rsp) = reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_load_resume);
    rip = reinterpret_cast<std::uint64_t>(&eden_fastmem_probe_fallback);
}

// Guest pages [0, 4 GiB) aliased at 16 KiB granularity into one reserved window: a chunk
// is aliased (read-write) when its four guest pages map consecutive backing pages that
// start on a 16 KiB backing boundary; everything else stays reserved. Access control is
// the per-page map byte the JIT tests before each direct access, so protection changes
// (GPU tracking, guest permissions) are byte stores, never system calls or faults.
class Window {
public:
    std::uint8_t* base{}; // window start (fastmem arena); the map occupies the MiB below
#ifdef PS5_NATIVE
    using Backing = std::int64_t; // direct-memory start of the backing
#else
    using Backing = int; // shared file descriptor of the backing
#endif

    ~Window() {
        if (!reservation) return;
        current_map = nullptr;
        if (munmap(reservation, ReserveBytes) != 0) Fatal("release", 0, 0);
#ifdef PS5_NATIVE
        if (map_physical >= 0 && sceKernelReleaseDirectMemory(map_physical, MapBytes) != 0) Fatal("map-release", 0, 0);
#endif
        counters.window = 0;
        counters.mapped_pages = 0;
        counters.aliased_chunks = 0;
    }

    bool Create(std::uint8_t* backing_base, Backing backing_handle) {
        backing = backing_handle;
#ifdef PS5_NATIVE
        // RADV's PS5 winsys owns [0x2_0000_0000, 0x3_0000_0000) (its 32-bit window) and places
        // device memory with fixed mappings in [0x40_0000_0000, 0x80_0000_0000); a window there
        // lost its access map to the first GPU allocations. Stay between the two.
        void* address = reinterpret_cast<void*>(0x1000000000ull);
        const auto reserved = sceKernelReserveVirtualRange(&address, ReserveBytes, 0, Granule);
        const auto placed = reinterpret_cast<std::uintptr_t>(address);
        if (reserved != 0 || placed < 0x300000000ull || placed + ReserveBytes > 0x4000000000ull) {
            std::printf("EDEN_FASTMEM_WINDOW reserve=%08x address=%p\n", unsigned(reserved), address);
            if (reserved == 0) munmap(address, ReserveBytes);
            return false;
        }
        reservation = static_cast<std::uint8_t*>(address);
        (void)sceKernelEnableDmemAliasing(); // Reports zero regardless; the self-test checks.
        void* map_at = reservation;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), MapBytes, Granule, 12, &map_physical) != 0 ||
            sceKernelMapDirectMemory(&map_at, MapBytes, PROT_READ | PROT_WRITE, MAP_FIXED, map_physical, Granule) != 0 ||
            map_at != reservation) {
            std::printf("EDEN_FASTMEM_WINDOW map=failed\n");
            return Abandon();
        }
#else
        void* address = mmap(nullptr, ReserveBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (address == MAP_FAILED) return false;
        reservation = static_cast<std::uint8_t*>(address);
        if (mmap(reservation, MapBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
            reservation)
            return Abandon();
#endif
        map = reservation;
        base = reservation + MapBytes;
        std::memset(map, Blocked, MapBytes);
        try {
            page_backing.assign(GuestPages, 0);
            page_access.assign(GuestPages, 0);
            chunk_aliased.assign(Chunks, 0);
            chunk_granule.assign(Chunks, 0);
        } catch (const std::bad_alloc&) {
            std::printf("EDEN_FASTMEM_WINDOW state=allocation\n");
            return Abandon();
        }
        if (const char* failure = SelfTest(backing_base)) {
            std::printf("EDEN_FASTMEM_CHECK ok=0 step=%s errno=%d\n", failure, errno);
            return Abandon();
        }
        counters.window = reinterpret_cast<std::uintptr_t>(base);
        current_map = map;
        return true;
    }

    void Map(std::size_t offset, std::size_t host_offset, std::size_t length, std::uint8_t access) {
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.map_calls;
        const std::size_t host = host_offset / GuestPage;
        for (std::size_t p = first; p <= last; ++p) {
            if (!page_backing[p]) ++counters.mapped_pages;
            page_backing[p] = static_cast<std::uint32_t>(host + (p - first) + 1);
            page_access[p] = access;
        }
        Update(first / PagesPerGranule, last / PagesPerGranule);
    }

    void Unmap(std::size_t offset, std::size_t length) {
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.unmap_calls;
        for (std::size_t p = first; p <= last; ++p) {
            if (page_backing[p]) --counters.mapped_pages;
            page_backing[p] = 0;
            page_access[p] = 0;
        }
        Update(first / PagesPerGranule, last / PagesPerGranule);
    }

    void Protect(std::size_t offset, std::size_t length, std::uint8_t access) {
        std::size_t first, last;
        if (!Pages(offset, length, first, last)) return;
        std::lock_guard lock{mutex};
        ++counters.protect_calls;
        std::fill(page_access.begin() + first, page_access.begin() + last + 1, access);
        Refresh(first, last);
    }

private:
    std::mutex mutex;
    std::uint8_t* reservation{};
    std::uint8_t* map{};
#ifdef PS5_NATIVE
    std::int64_t map_physical = -1;
#endif
    Backing backing{};
    std::vector<std::uint32_t> page_backing; // backing page + 1; 0 = unmapped
    std::vector<std::uint8_t> page_access;   // Readable | Writable as Eden requested
    std::vector<std::uint8_t> chunk_aliased;
    std::vector<std::uint32_t> chunk_granule; // backing granule of an aliased chunk

    bool Abandon() {
        munmap(reservation, ReserveBytes);
#ifdef PS5_NATIVE
        if (map_physical >= 0) sceKernelReleaseDirectMemory(map_physical, MapBytes);
        map_physical = -1;
#endif
        reservation = map = base = nullptr;
        return false;
    }

    // Inclusive guest page range of a request, clipped to the window.
    static bool Pages(std::size_t offset, std::size_t length, std::size_t& first, std::size_t& last) {
        if (offset >= WindowBytes || !length) return false;
        first = offset / GuestPage;
        last = (std::min(offset + length, WindowBytes) - 1) / GuestPage;
        return true;
    }

    bool Aliasable(std::size_t chunk, std::uint32_t& granule) const {
        const std::size_t page = chunk * PagesPerGranule;
        const std::uint32_t entry = page_backing[page];
        if (!entry || (entry - 1) % PagesPerGranule) return false;
        for (std::size_t i = 1; i < PagesPerGranule; ++i)
            if (page_backing[page + i] != entry + i) return false;
        granule = (entry - 1) / PagesPerGranule;
        return true;
    }

    // Blocked bits of a page on its own; pages past the window are blocked.
    std::uint8_t Own(std::size_t page) const {
        if (page >= GuestPages || !chunk_aliased[page / PagesPerGranule]) return Blocked;
        const auto access = page_access[page];
        return ((access & Readable) ? 0 : ReadBlocked) | ((access & Writable) ? 0 : WriteBlocked);
    }
    // A page's byte also carries its successor's blocks: direct accesses may cross into it.
    void Refresh(std::size_t first, std::size_t last) {
        for (std::size_t page = first ? first - 1 : 0; page <= last; ++page)
            map[page] = Own(page) | Own(page + 1);
    }
    void Block(std::size_t chunk, std::size_t count) {
        const std::size_t first = chunk * PagesPerGranule, last = (chunk + count) * PagesPerGranule - 1;
        for (std::size_t page = first ? first - 1 : 0; page <= last; ++page) map[page] = Blocked;
    }

    template<class Operation>
    bool Kernel(Operation&& operation) {
        const auto start = std::chrono::steady_clock::now();
        const bool ok = operation();
        counters.kernel_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        ++counters.kernel_calls;
        return ok;
    }
    bool MapChunks(std::size_t chunk, std::size_t count, std::uint32_t granule) {
        auto* at = base + chunk * Granule;
        const std::size_t bytes = count * Granule;
        return Kernel([&] {
#ifdef PS5_NATIVE
            void* address = at;
            return sceKernelMapDirectMemory(&address, bytes, PROT_READ | PROT_WRITE, MAP_FIXED,
                                            backing + std::int64_t(granule) * std::int64_t(Granule),
                                            Granule) == 0 &&
                   address == at;
#else
            return mmap(at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, backing,
                        off_t(granule) * off_t(Granule)) == at;
#endif
        });
    }
    bool UnmapChunks(std::size_t chunk, std::size_t count) {
        auto* at = base + chunk * Granule;
        const std::size_t bytes = count * Granule;
        return Kernel([&] {
#ifdef PS5_NATIVE
            // In one call (memory_pages.cpp): unmapped first, the chunks were free for a moment.
            return Common::ReserveOver(at, bytes, Granule) == 1;
#else
            return mmap(at, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                        -1, 0) == at;
#endif
        });
    }

    void Update(std::size_t first, std::size_t last) {
        // 1. Withdraw aliases that no longer match their pages; block their bytes first.
        std::size_t run = 0, count = 0;
        const auto withdraw = [&] {
            if (!count) return;
            Block(run, count);
            if (!UnmapChunks(run, count)) Fatal("unmap", run, count);
            for (std::size_t c = run; c < run + count; ++c) chunk_aliased[c] = 0;
            counters.aliased_chunks -= count;
            count = 0;
        };
        for (std::size_t c = first; c <= last; ++c) {
            std::uint32_t granule = 0;
            if (chunk_aliased[c] && (!Aliasable(c, granule) || granule != chunk_granule[c])) {
                if (count++ == 0) run = c;
            } else {
                withdraw();
            }
        }
        withdraw();

        // 2. Alias chunks that became aliasable, in runs of consecutive backing granules.
        std::uint32_t run_granule = 0;
        const auto alias = [&] {
            if (!count) return;
            if (MapChunks(run, count, run_granule)) {
                for (std::size_t i = 0; i < count; ++i) {
                    chunk_aliased[run + i] = 1;
                    chunk_granule[run + i] = run_granule + static_cast<std::uint32_t>(i);
                }
                counters.aliased_chunks += count;
            } else {
                ++counters.failures; // Left reserved: accesses keep the page-table path.
            }
            count = 0;
        };
        for (std::size_t c = first; c <= last; ++c) {
            std::uint32_t granule = 0;
            const bool candidate = !chunk_aliased[c] && Aliasable(c, granule);
            if (candidate && count && granule == run_granule + count) {
                ++count;
                continue;
            }
            alias();
            if (candidate) {
                run = c;
                run_granule = granule;
                count = 1;
            }
        }
        alias();

        // 3. Publish access for every page of the updated chunks (and the page before).
        Refresh(first * PagesPerGranule, (last + 1) * PagesPerGranule - 1);
    }

    // Exercise the kernel operations the window relies on, with the fake call the JIT
    // fault handler makes, before any guest code runs. Returns the failing step.
    const char* SelfTest(std::uint8_t* backing_base) {
        struct sigaction action{}, previous_segv{}, previous_bus{};
        action.sa_sigaction = ProbeHandler;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, &previous_segv) != 0) return "install";
        if (sigaction(SIGBUS, &action, &previous_bus) != 0) {
            sigaction(SIGSEGV, &previous_segv, nullptr);
            return "install";
        }
        const char* failure = SelfTestSteps(backing_base);
        sigaction(SIGBUS, &previous_bus, nullptr);
        sigaction(SIGSEGV, &previous_segv, nullptr);
        return failure;
    }
    const char* SelfTestSteps(std::uint8_t* backing_base) {
        constexpr std::uint64_t pattern = 0x0123456789abcdefull;
        auto* word = reinterpret_cast<volatile std::uint64_t*>(backing_base + Granule + 8);
        std::uint64_t rsp = 0;
        if (map[MapBytes - 1] != Blocked) return "map";
        // Reserved memory faults; the fake call returns after the faulting load.
        probe = {};
        if (eden_fastmem_probe_load(base, &rsp) != ProbeMarker || probe.hits != 1) return "reserved-fault";
        if (probe.rsp != rsp) return "context-rsp";
        const int signal = probe.signal;
        // Two chunks alias the first two backing granules.
        if (!MapChunks(0, 2, 0)) return "alias";
        *word = pattern;
        if (eden_fastmem_probe_load(base + Granule + 8, &rsp) != pattern || probe.hits != 1) return "alias-read";
        // Unmapping one chunk re-reserves it and keeps its neighbour.
        if (!UnmapChunks(0, 1)) return "partial-unmap";
        if (eden_fastmem_probe_load(base + 8, &rsp) != ProbeMarker || probe.hits != 2) return "unmapped-fault";
        if (eden_fastmem_probe_load(base + Granule + 8, &rsp) != pattern || probe.hits != 2)
            return "neighbour-read";
        if (!UnmapChunks(1, 1)) return "unmap";
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 16; ++i) eden_fastmem_probe_load(base + Granule + 8, &rsp);
        const auto fault_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count() / 16;
        if (probe.hits != 18) return "final-fault";
        *word = 0;
        std::printf("EDEN_FASTMEM_CHECK ok=1 window=%p signal=%d fault_ns=%lld kernel_calls=%llu\n",
                    static_cast<void*>(base), signal, static_cast<long long>(fault_ns),
                    static_cast<unsigned long long>(counters.kernel_calls.load()));
        counters.kernel_calls = 0;
        counters.kernel_ns = 0;
        return nullptr;
    }
};
} // namespace

namespace {
// ---- The 64-bit window (EDEN_FASTMEM64=1; backported from ProsperoEden, GPL-3.0-or-later) ----
// The page table's fastmem arena for 64-bit games, shown lazily. A JIT access faults on a 2 MiB
// region that shows nothing. If all 512 pages of that guest region map one 2 MiB-aligned stretch of
// the guest's memory with the same access, the slot there is shown as one large page
// (guest_memory_service.inc, AliasGuestSlot) and the access repeats. A region that cannot be shown
// whole shows the 16 KiB piece around the access when its four pages are mapped in line. Otherwise
// the JIT's fallback runs and dynarmic moves that access site onto the page-table path. 2^38 bytes
// cover every guest address a 64-bit title uses; the JIT range-checks to that (Window64Bits).
std::atomic<bool> window64_requested{false};
std::atomic<std::uint64_t> window64_base{0};
struct Window64Counts {
    std::atomic<std::uint64_t> whole, pieces, read_only, promoted;
    std::atomic<std::uint64_t> no_region, partial, inconsistent, blocked, blocked_write, unaligned, slot_refused;
    std::atomic<std::uint64_t> piece_refused, piece_cap, small_regions;
    // Why a piece was not shown: a page of it unmapped, read-blocked (or write-blocked for a write),
    // its pages not in line in the backing, the backing off 16 KiB, or the slot refused it.
    std::atomic<std::uint64_t> piece_unmapped, piece_blocked, piece_scattered, piece_misaligned, piece_slot;
    // Misaligned pieces by the guest's 4 GiB area (address >> 32), and how far off 16 KiB their
    // backing is (in 4 KiB pages: 1, 2 or 3): which memory keeps sites off the window.
    std::array<std::atomic<std::uint64_t>, 64> misaligned_area;
    std::array<std::atomic<std::uint64_t>, 4> misaligned_phase;
    std::atomic<std::uint64_t> drop_map, drop_unmap, drop_protect;
} window64_counts;

constexpr std::size_t Window64Bytes = std::size_t{1} << Eden::Fastmem::Window64Bits;
struct Window64 {
    static constexpr std::size_t Region = 0x200000, RegionPages = Region / GuestPage;
    static constexpr std::size_t Piece = 0x4000, PiecePages = Piece / GuestPage, Pieces = Region / Piece;
    struct State {
        std::uintptr_t start = 0;          // the backing address the region's first page maps to
        bool consistent = true;            // every mapped page agrees with `start`
        // When not: each page's backing page (from backing_base) + 1, 0 = none.
        std::unique_ptr<std::uint32_t[]> backing_pages;
        std::vector<std::uintptr_t> piece_backings; // one backing address per 2 MiB slot that pieces show
        bool small = false;                // shows 16 KiB pieces (AliasGuestPage)
        std::bitset<Pieces> pieces;        // which pieces are shown
        std::array<std::uint8_t, Pieces> piece_protection{}; // each shown piece's PROT_* bits
        std::bitset<RegionPages> mapped, read_blocked, write_blocked; // Eden's access, page by page
    };
    std::uint8_t* base{};
    std::uintptr_t backing_base{};
    std::mutex mutex;
    std::unordered_map<std::uint32_t, State> regions;

    // Each region shown in pieces costs one of the title's 16,384 CPU page tables, which the game,
    // the GPU driver and the window share: a new one is refused once fewer than PageTablesKept are
    // free (a fixed cap where the count is unavailable).
    static constexpr std::uint64_t SmallRegionCap = 768;
    static constexpr int PageTablesKept = 1024;
    static bool PageTablesLeft(std::uint64_t small_regions) {
#ifdef PS5_NATIVE
        int cpu_tables = 0, cpu_free = 0, gpu_tables = 0, gpu_free = 0;
        if (sceKernelGetPageTableStats(&cpu_tables, &cpu_free, &gpu_tables, &gpu_free) == 0)
            return cpu_free > PageTablesKept;
#endif
        return small_regions < SmallRegionCap;
    }
    std::uintptr_t RegionAt(std::uint32_t region) const {
        return reinterpret_cast<std::uintptr_t>(base) + std::uintptr_t(region) * Region;
    }
    void Drop(std::uint32_t region, State& state, std::atomic<std::uint64_t>& why) {
        const auto window_at = RegionAt(region);
        bool dropped = state.start && Common::DropGuestAlias(state.start, window_at);
        if (state.small) {
            // Pieces may come from other slots: a region off a 2 MiB boundary spans two.
            for (const std::uintptr_t backing : state.piece_backings)
                dropped |= Common::DropGuestAlias(backing, window_at);
            state.piece_backings.clear();
            state.small = false;
            state.pieces.reset();
            window64_counts.small_regions.fetch_sub(1, std::memory_order_relaxed);
        }
        if (dropped) why.fetch_add(1, std::memory_order_relaxed);
    }
    // A slot without a block of its own, written through the window: take one as a write to the
    // backing would (guest_memory_service.inc).
    static bool Promote(std::uintptr_t backing) {
        const auto take = Eden::Fastmem::guest_write_fault.load(std::memory_order_acquire);
        if (!take || !take(reinterpret_cast<void*>(backing))) return false;
        window64_counts.promoted.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // The 16 KiB holding `address` on its own, when its four guest pages are mapped, in line in the
    // backing and readable (and writable, for a write; else it is shown read-only).
    bool ShowPiece(std::uint32_t region, State& state, std::uintptr_t address, bool write) {
        const auto region_at = RegionAt(region);
        const std::size_t first = (address - region_at) / Piece * PiecePages;
        bool writable = true;
        auto& counts = window64_counts;
        // A piece may hold unmapped pages beside its mapped ones (a stack's guard page, a lone TLS
        // page): they show the backing that lines up with the piece, which only an access the
        // guest's own page table would refuse can reach. Refusing them kept ~450k accesses of an
        // MKW session off the window. The accessed page itself must be mapped.
        // EDEN_FASTMEM64_STRICT_PIECES=1 refuses such pieces (as ProsperoEden's switch of that name).
        static const bool strict = [] {
            const char* value = std::getenv("EDEN_FASTMEM64_STRICT_PIECES");
            return value != nullptr && value[0] == '1';
        }();
        if (!state.mapped[(address - region_at) / GuestPage])
            return counts.piece_unmapped.fetch_add(1, std::memory_order_relaxed), false;
        for (std::size_t i = first; i < first + PiecePages; ++i) {
            if (!state.mapped[i]) {
                if (strict) return counts.piece_unmapped.fetch_add(1, std::memory_order_relaxed), false;
                continue;
            }
            if (state.read_blocked[i]) return counts.piece_blocked.fetch_add(1, std::memory_order_relaxed), false;
            writable &= !state.write_blocked[i];
        }
        if (write && !writable) return counts.piece_blocked.fetch_add(1, std::memory_order_relaxed), false;
        std::uintptr_t backing = state.start + first * GuestPage;
        if (!state.consistent) {
            backing = 0;
            for (std::size_t i = first; i < first + PiecePages; ++i) {
                if (!state.mapped[i]) continue;  // shown in line with the mapped ones
                const std::uintptr_t at = backing_base + std::uintptr_t(state.backing_pages[i] - 1) * GuestPage -
                                          (i - first) * GuestPage;
                if (!state.backing_pages[i] || (backing && at != backing))
                    return counts.piece_scattered.fetch_add(1, std::memory_order_relaxed), false;
                backing = at;
            }
        }
        if (const std::size_t phase = (backing - backing_base) % Piece) {
            counts.misaligned_area[((address - reinterpret_cast<std::uintptr_t>(base)) >> 32) % 64].fetch_add(1, std::memory_order_relaxed);
            counts.misaligned_phase[phase / GuestPage % 4].fetch_add(1, std::memory_order_relaxed);
            return counts.piece_misaligned.fetch_add(1, std::memory_order_relaxed), false;
        }
        if (!state.small && !PageTablesLeft(window64_counts.small_regions.load(std::memory_order_relaxed)))
            return window64_counts.piece_cap.fetch_add(1, std::memory_order_relaxed), false;
        bool promote = false;
        const std::uintptr_t at = region_at + first * GuestPage;
        if (!Common::AliasGuestPage(backing, at, region_at, writable, write, &promote)) {
            if (!promote || !Promote(backing) || (promote = false, !Common::AliasGuestPage(backing, at, region_at, writable, write, &promote)))
                return counts.piece_slot.fetch_add(1, std::memory_order_relaxed), false;
            // The promotion voided the region's other pieces (DropSlotAliases).
            state.pieces.reset();
        }
        if (!state.small) {
            state.small = true;
            window64_counts.small_regions.fetch_add(1, std::memory_order_relaxed);
        }
        state.pieces.set(first / PiecePages);
        state.piece_protection[first / PiecePages] = static_cast<std::uint8_t>(writable ? PROT_READ | PROT_WRITE : PROT_READ);
        if (std::find_if(state.piece_backings.begin(), state.piece_backings.end(),
                         [backing](std::uintptr_t b) { return (b >> 21) == (backing >> 21); }) == state.piece_backings.end())
            state.piece_backings.push_back(backing);
        window64_counts.pieces.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    template <class Each>
    void ForRegions(std::size_t offset, std::size_t length, Each&& each) {
        if (offset >= Window64Bytes || !length) return;
        const std::size_t end = std::min(offset + length, Window64Bytes);
        for (std::size_t region = offset / Region; region * Region < end; ++region) {
            const std::size_t first = std::max(offset, region * Region), last = std::min(end, (region + 1) * Region);
            each(static_cast<std::uint32_t>(region), (first - region * Region) / GuestPage, (last - first) / GuestPage, first);
        }
    }
    void Map(std::size_t offset, std::size_t host_offset, std::size_t length, std::uint8_t access) {
        std::lock_guard lock{mutex};
        ForRegions(offset, length, [&](std::uint32_t region, std::size_t page, std::size_t pages, std::size_t first) {
            auto& state = regions[region];
            Drop(region, state, window64_counts.drop_map);
            const std::uintptr_t start = backing_base + host_offset + (first - offset) - (first - region * Region);
            if (state.mapped.none()) {
                state.start = start;
                state.consistent = true;
                state.backing_pages.reset();
            } else if (state.start != start && state.consistent) {
                state.consistent = false;
                state.backing_pages = std::make_unique<std::uint32_t[]>(RegionPages);
                for (std::size_t i = 0; i < RegionPages; ++i)
                    if (state.mapped[i])
                        state.backing_pages[i] = std::uint32_t((state.start + i * GuestPage - backing_base) / GuestPage + 1);
            }
            for (std::size_t i = page; i < page + pages; ++i) {
                if (!state.consistent)
                    state.backing_pages[i] = std::uint32_t((start + i * GuestPage - backing_base) / GuestPage + 1);
                state.mapped.set(i);
                state.read_blocked.set(i, !(access & Readable));
                state.write_blocked.set(i, !(access & Writable));
            }
        });
    }
    void Unmap(std::size_t offset, std::size_t length) {
        std::lock_guard lock{mutex};
        ForRegions(offset, length, [&](std::uint32_t region, std::size_t page, std::size_t pages, std::size_t) {
            const auto it = regions.find(region);
            if (it == regions.end()) return;
            Drop(region, it->second, window64_counts.drop_unmap);
            for (std::size_t i = page; i < page + pages; ++i) {
                it->second.mapped.reset(i);
                if (it->second.backing_pages) it->second.backing_pages[i] = 0;
            }
            if (it->second.mapped.none()) regions.erase(it);
        });
    }
    void Protect(std::size_t offset, std::size_t length, std::uint8_t access) {
        std::lock_guard lock{mutex};
        ForRegions(offset, length, [&](std::uint32_t region, std::size_t page, std::size_t pages, std::size_t) {
            const auto it = regions.find(region);
            if (it == regions.end()) return;
            State& state = it->second;
            // A region shown in pieces changes only the pieces the pages are in: GPU write tracking
            // protects pages hundreds of times a second.
            if (!state.small && access != (Readable | Writable)) Drop(region, state, window64_counts.drop_protect);
            for (std::size_t i = page; i < page + pages; ++i) {
                state.read_blocked.set(i, !(access & Readable));
                state.write_blocked.set(i, !(access & Writable));
            }
            if (!state.small) return;
            const auto region_at = RegionAt(region);
            for (std::size_t piece = page / PiecePages; piece * PiecePages < page + pages; ++piece) {
                if (!state.pieces[piece]) continue;
                bool readable = true, writable = true;
                for (std::size_t i = piece * PiecePages; i < (piece + 1) * PiecePages; ++i) {
                    if (!state.mapped[i]) continue;  // an unmapped page's bits are stale
                    readable &= !state.read_blocked[i];
                    writable &= !state.write_blocked[i];
                }
                // A piece only becomes more permissive through a fault (ShowPiece), never here.
                const int shown = state.piece_protection[piece];
                const int protection = !readable ? PROT_NONE : writable ? shown : PROT_READ;
                if (protection == shown) continue;
                state.piece_protection[piece] = static_cast<std::uint8_t>(protection);
                if (mprotect(reinterpret_cast<void*>(region_at + piece * Piece), Piece, protection) != 0) {
                    Drop(region, state, window64_counts.drop_protect);
                    return;
                }
            }
        });
    }
    // From the fault handler (a JIT thread, which never holds `mutex` while it runs guest code).
    // A region whose only blocked pages are write-blocked (GPU write tracking) is shown read-only:
    // reads go direct and a write faults again, to the page-table path that tracks it.
    bool Fault(std::uintptr_t address, bool write) {
        const auto at = reinterpret_cast<std::uintptr_t>(base);
        if (address < at || address >= at + Window64Bytes) return false;
        const auto region = static_cast<std::uint32_t>((address - at) / Region);
        std::lock_guard lock{mutex};
        auto& counts = window64_counts;
        const auto it = regions.find(region);
        if (it == regions.end()) return counts.no_region.fetch_add(1, std::memory_order_relaxed), false;
        State& state = it->second;
        const auto refuse = [&](std::atomic<std::uint64_t>& count) {
            if (ShowPiece(region, state, address, write)) return true;
            count.fetch_add(1, std::memory_order_relaxed);
            return false;
        };
        if (!state.mapped.all()) return refuse(counts.partial);
        if (!state.consistent) return refuse(counts.inconsistent);
        if (state.read_blocked.any()) return refuse(counts.blocked);
        const bool writable = state.write_blocked.none();
        if (write && !writable) return refuse(counts.blocked_write);
        if ((state.start - backing_base) % Region) return refuse(counts.unaligned);
        bool promote = false;
        bool shown = Common::AliasGuestSlot(state.start, RegionAt(region), writable, write, &promote);
        if (!shown && promote && Promote(state.start + (address - RegionAt(region)))) {
            promote = false;
            shown = Common::AliasGuestSlot(state.start, RegionAt(region), writable, write, &promote);
        }
        if (!shown) return refuse(counts.slot_refused);
        if (state.small) {
            state.small = false;
            state.pieces.reset();
            state.piece_backings.clear();
            counts.small_regions.fetch_sub(1, std::memory_order_relaxed);
        }
        counts.whole.fetch_add(1, std::memory_order_relaxed);
        if (!writable) counts.read_only.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bool Create() {
#ifdef PS5_NATIVE
        (void)sceKernelEnableDmemAliasing();
        // Above the guest's own memory (0x80_0000_0000) and never in RADV's device-memory range
        // [0x40_0000_0000, 0x80_0000_0000), where it maps with fixed addresses.
        void* address = reinterpret_cast<void*>(0xa000000000ull);
        if (sceKernelReserveVirtualRange(&address, Window64Bytes, 0, Region) != 0) return false;
        if (reinterpret_cast<std::uintptr_t>(address) < 0x8000000000ull) {
            munmap(address, Window64Bytes);
            return false;
        }
#else
        void* address = mmap(nullptr, Window64Bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (address == MAP_FAILED) return false;
#endif
        base = static_cast<std::uint8_t*>(address);
        window64_base = reinterpret_cast<std::uintptr_t>(base);
        return true;
    }
    ~Window64() {
        Eden::Fastmem::window64_fault.store(nullptr, std::memory_order_release);
        {
            std::lock_guard lock{mutex};
            for (auto& [region, state] : regions) Drop(region, state, window64_counts.drop_unmap);
            regions.clear();
        }
        if (base && munmap(base, Window64Bytes) != 0) std::abort();
        window64_base = 0;
    }
};
std::atomic<Window64*> window64_current{nullptr};
bool Window64Fault(void* address, bool write) noexcept {
    auto* window = window64_current.load(std::memory_order_acquire);
    return window && window->Fault(reinterpret_cast<std::uintptr_t>(address), write);
}
} // namespace

namespace Eden::Fastmem {
void Request(bool enabled) noexcept { window_requested = enabled; }
bool Requested() noexcept { return window_requested; }
void RequestA64(bool enabled) noexcept { window64_requested = enabled; }
bool RequestedA64() noexcept { return window64_requested; }
std::uint64_t Window64Base() noexcept { return window64_base; }
// Page-reuse groups formed while blocks compile (headless/dynarmic/page_reuse_pass.inc).
extern "C" unsigned long long eden_page_reuse_groups __attribute__((weak));
void Window64Report(char* out, std::size_t size) noexcept {
    auto& c = window64_counts;
    std::uint64_t made = 0, dropped = 0, refused = 0;
    Common::GuestAliasCounts(&made, &dropped, &refused);
    const auto v = [](const std::atomic<std::uint64_t>& a) { return static_cast<unsigned long long>(a.load(std::memory_order_relaxed)); };
    std::snprintf(out, size,
                  "EDEN_FASTMEM64 whole=%llu pieces=%llu read_only=%llu promoted=%llu refused: no_region=%llu partial=%llu "
                  "inconsistent=%llu blocked=%llu blocked_write=%llu unaligned=%llu slot=%llu piece_cap=%llu "
                  "small_regions=%llu piece_refused: unmapped=%llu blocked=%llu scattered=%llu misaligned=%llu slot=%llu drops: map=%llu unmap=%llu protect=%llu aliases: made=%llu dropped=%llu refused=%llu",
                  v(c.whole), v(c.pieces), v(c.read_only), v(c.promoted), v(c.no_region), v(c.partial), v(c.inconsistent),
                  v(c.blocked), v(c.blocked_write), v(c.unaligned), v(c.slot_refused), v(c.piece_cap),
                  v(c.small_regions), v(c.piece_unmapped), v(c.piece_blocked), v(c.piece_scattered), v(c.piece_misaligned),
                  v(c.piece_slot), v(c.drop_map), v(c.drop_unmap), v(c.drop_protect),
                  static_cast<unsigned long long>(made), static_cast<unsigned long long>(dropped),
                  static_cast<unsigned long long>(refused));
    std::size_t used = std::strlen(out);
    if (&eden_page_reuse_groups != nullptr && used < size)
        used += std::snprintf(out + used, size - used, " reuse_groups=%llu", __atomic_load_n(&eden_page_reuse_groups, __ATOMIC_RELAXED));
    if (used < size)
        used += std::snprintf(out + used, size - used, " misaligned_phase=%llu/%llu/%llu areas=", v(c.misaligned_phase[1]),
                              v(c.misaligned_phase[2]), v(c.misaligned_phase[3]));
    for (std::size_t area = 0; area < c.misaligned_area.size() && used < size; ++area)
        if (const auto n = v(c.misaligned_area[area]))
            used += std::snprintf(out + used, size - used, "%zx:%llu,", area, n);
}
Stats WindowStats() noexcept {
    std::uint64_t direct_reads = 0, direct_writes = 0;
    if (const auto* map = current_map.load()) {
        for (std::size_t page = 0; page < GuestPages; ++page) {
            direct_reads += !(map[page] & ReadBlocked);
            direct_writes += !(map[page] & WriteBlocked);
        }
    }
    return {counters.window, counters.mapped_pages, counters.aliased_chunks, direct_reads, direct_writes,
            counters.map_calls, counters.unmap_calls, counters.protect_calls,
            counters.kernel_calls, counters.kernel_ns, counters.failures};
}
} // namespace Eden::Fastmem

namespace Common {
// ponytail: backing plus, on request, the A32 fastmem window above. NCE is unavailable.
class HostMemory::Impl {
public:
    u8* base{};
    size_t size;
    std::unique_ptr<Window> window;
    std::unique_ptr<Window64> window64;
#ifndef PS5_NATIVE
    int fd = -1; // Host checks: a shared backing the window can alias.
#endif
    Impl(size_t requested, bool fastmem) : size(requested) {
#ifndef PS5_NATIVE
        if (fastmem) {
            fd = memfd_create("eden-backing", MFD_CLOEXEC);
            if (fd < 0 || ftruncate(fd, off_t(requested)) != 0)
                throw std::system_error(errno, std::generic_category(), "Backing file");
            void* mapped = mmap(nullptr, requested, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (mapped == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "Backing map");
            base = static_cast<u8*>(mapped);
        }
#endif
        // The fastmem window aliases the block by its place in direct memory: it is then one block.
        if (!base)
            base = static_cast<u8*>(fastmem ? AllocateGuestMemory(requested) : AllocateGuestMemoryWhereUsed(requested));
        if (!base) throw std::system_error(errno, std::generic_category(), "Backing allocation");
        if (!fastmem) return;
        window = std::make_unique<Window>();
#ifdef PS5_NATIVE
        const Window::Backing handle = DirectMemoryStart(base);
#else
        const Window::Backing handle = fd;
#endif
        if (!window->Create(base, handle)) window.reset();
    }
    ~Impl() {
        window.reset();
        // Its aliases go before the guest's memory does.
        window64_current.store(nullptr, std::memory_order_release);
        window64.reset();
#ifndef PS5_NATIVE
        if (fd >= 0) {
            if (munmap(base, size) != 0 || close(fd) != 0) std::abort();
            return;
        }
#endif
        FreeGuestMemory(base, size);
    }
};

#ifdef EDEN_HOST_MEMORY_BACKING_MODE
HostMemory::HostMemory(size_t size, size_t virtual_size_, BackingMode)
#else
HostMemory::HostMemory(size_t size, size_t virtual_size_)
#endif
    : backing_size(size),
      impl(std::make_unique<Impl>(size, virtual_size_ && Eden::Fastmem::Requested())),
      backing_base(impl->base) {
    if (impl->window) {
        virtual_base = impl->window->base;
        virtual_size = WindowBytes;
    }
    if (virtual_size_ && !virtual_base && Eden::Fastmem::RequestedA64()) {
        impl->window64 = std::make_unique<Window64>();
        if (impl->window64->Create()) {
            virtual_base = impl->window64->base;
            virtual_size = Window64Bytes;
            impl->window64->backing_base = reinterpret_cast<std::uintptr_t>(backing_base);
            window64_current.store(impl->window64.get(), std::memory_order_release);
            Eden::Fastmem::window64_fault.store(&Window64Fault, std::memory_order_release);
        } else {
            impl->window64.reset();
        }
        std::printf("EDEN_FASTMEM64 window=%p\n", static_cast<void*>(virtual_base));
        std::fflush(stdout);
    }
}
HostMemory::~HostMemory() = default;
HostMemory::HostMemory(HostMemory&& other) noexcept { *this = std::move(other); }
HostMemory& HostMemory::operator=(HostMemory&& other) noexcept {
    if (this != &other) {
        impl = std::move(other.impl);
        backing_size = std::exchange(other.backing_size, 0);
        backing_base = std::exchange(other.backing_base, nullptr);
        virtual_base = std::exchange(other.virtual_base, nullptr);
        virtual_size = std::exchange(other.virtual_size, 0);
    }
    return *this;
}
void HostMemory::ClearBackingRegion(size_t offset, size_t length, u32 fill) {
    if (offset > backing_size || length > backing_size - offset)
        throw std::out_of_range("Backing clear range");
    // A fill with zeroes leaves a slot that shows the zero block alone (memory_pages.cpp).
    if (length) ClearGuestMemory(backing_base + offset, offset, length, static_cast<int>(fill));
}
#if defined(EDEN_HOST_MEMORY_BACKING_MODE) || defined(EDEN_HOST_MEMORY_ZERO_TRACKING)
// PS5 clears unconditionally through its demand-backed guest-memory service.
// Upstream's skip-zero tracking is therefore unnecessary here.
void HostMemory::MarkUsed(size_t, size_t) {}
void HostMemory::MarkDirty(size_t, size_t) {}
void HostMemory::ForgetContents() {}
#endif
#ifdef EDEN_HOST_MEMORY_COLD_HINT
// Direct-memory backing cannot be paged out; retain its contents until the guest clears it.
void HostMemory::HintCold(size_t, size_t, std::optional<size_t>) const {}
#endif
// Eden asks for these while a page table uses VirtualBasePointer() as its fastmem arena. Without a window (the
// window is off on the console) there is no arena to change: the page table, which Eden has already updated, is what
// serves the guest's memory, so the request has nothing left to do. It used to be an error; a Switch 2 program made
// the session fail with it before it had started. The first one is written to the log.
static void NoWindow(const char* what) noexcept {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) {
        std::fprintf(stderr, "EDEN_HOST_MEMORY %s without a fastmem window: ignored\n", what);
        std::fflush(stderr);
    }
}
void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms, bool) {
    if (impl && impl->window64) return impl->window64->Map(virtual_offset, host_offset, length, Access(perms));
    if (!impl || !impl->window) return NoWindow("map");
    if (host_offset > backing_size || length > backing_size - host_offset)
        throw std::out_of_range("Backing map range");
    impl->window->Map(virtual_offset, host_offset, length, Access(perms));
}
void HostMemory::Unmap(size_t virtual_offset, size_t length, bool) {
    if (impl && impl->window64) return impl->window64->Unmap(virtual_offset, length);
    if (!impl || !impl->window) return NoWindow("unmap");
    impl->window->Unmap(virtual_offset, length);
}
void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perms) {
    if (impl && impl->window64) return impl->window64->Protect(virtual_offset, length, Access(perms));
    if (!impl || !impl->window) return NoWindow("protect");
    impl->window->Protect(virtual_offset, length, Access(perms));
}
void HostMemory::EnableDirectMappedAddress() {
    throw std::logic_error("NCE unavailable in x64 backing-only adapter");
}
} // namespace Common
