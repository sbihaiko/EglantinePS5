// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "video_core/nx2_profile.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    static bool IsBackThread() {
        return back_thread_context;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Called on the back before it sleeps waiting for work (the rasterizer submits deferred work).
    void SetIdleHook(void (*hook)(void*), void* argument) {
        idle_hook = hook;
        idle_argument = argument;
    }
    /// Any thread: has the back call the idle hook soon, between entries or at once if it sleeps.
    void RequestIdleHook() {
        hook_requested.store(true, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment);
    /// Whether plain register writes travel in blocks (PlainWrite); EDEN_NX2_REG_BLOCKS=0 turns it off.
    [[nodiscard]] bool RegBlocks() const {
        return reg_blocks;
    }
    /// A state register write with no side effect (not in the engine's execution_mask): gathered
    /// with the ones around it into one Regs entry, which the back applies through the engine's
    /// method sink (ProcessShadowRam, same-value skip, dirty flags) before anything sent after it.
    void PlainWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        BlockWrite(Kind::Regs, engine, method, argument);
    }
    /// A 3D constant-buffer data write (CB_DATA): gathered with the ones next to it into one
    /// CbData entry, which the back runs as that many writes of the register (shadow RAM, dirty
    /// flags) and one guest-memory write (Maxwell3D::ProcessCBDataBlock).
    void CbDataWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        BlockWrite(Kind::CbData, engine, method, argument);
    }
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1);
    }

    /// Defer flag: the call can't change what a draw resolves to, so it doesn't end the fast
    /// draw delta chain (nx2_fast_draw.h). Every other deferred call does.
    static constexpr u64 FastDrawNeutral = 1 << 7;

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    /// `flags`: FastDrawNeutral when `func` can't change what a draw resolves to.
    template <typename Func>
    void Defer(const Func& func, u64 flags = 0) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | flags | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
    }
    void DeferFunction(std::function<void()>&& func, u64 flags = 0);

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Front: a semaphore release just sent to the back; `key`: address space and GPU address.
    void NoteRelease(u64 key, u32 value) {
        front_releases[key] = {value, write_position};
    }
    /// Front: a QMD release that leaves at least `value` at `key` (a "max" release), as the
    /// front's copy of the QMD scheduling read it.
    void NoteLowerBound(u64 key, u32 value) {
        front_lower_bounds[key] = {value, write_position};
    }
    /// Front: a QMD release that adds `value` to the word at `key`.
    void NoteAdd(u64 key, u32 value) {
        front_adds[key].push_back({value, write_position});
    }
    /// Front: a lower bound of the word at `key` once the back has run everything sent so far,
    /// given `memory` (what it holds now): the adds and "max" releases the back has not run yet.
    /// Adds it ran whose fence has not landed count as not there (low, safe). For "greater or
    /// equal" acquires only; empty when nothing for `key` is pending.
    std::optional<u32> PendingLowerBound(u64 key, u32 memory);
    /// Front: a release to `key` whose value the front cannot know (a QMD's, read when it runs)
    /// was just sent to the back.
    void NotePending(u64 key) {
        front_pending[key] = write_position;
    }
    /// Front: where the back will have run the last such release to `key`, while it has not
    /// (DrainTo it: a partial drain).
    std::optional<u64> PendingPosition(u64 key);
    /// Front: what the last release noted for `key` writes, while the back may not have run it.
    /// An acquire that value meets needs no Drain: the back runs the release before anything the
    /// front sends after the acquire. Empty once the back ran it (memory then tells).
    std::optional<u32> PendingRelease(u64 key);
    /// Front: a report the back will write, `bytes` at `key` (address space and GPU address).
    void NoteReport(u64 key, u32 bytes) {
        for (u32 offset = 0; offset < bytes; offset += sizeof(u32)) {
            front_reports[key + offset] = write_position;
            reported_words.insert(key + offset);
        }
    }
    /// Front: whether a report was ever sent to the word at `key`. The back's query cache can hold
    /// its result after running it, until a fence writes it (MME DMA reads flush it).
    [[nodiscard]] bool EverReported(u64 key) const {
        return reported_words.contains(key);
    }
    /// Front: whether the back has yet to run a write the front sent to the word at `key`: a
    /// report, or a release noted above (exact, QMD, "add" or "max").
    [[nodiscard]] bool WritePending(u64 key);
    /// Front: a wait for idle (3D or puller) was sent: reads that run ahead of the back (the MME's
    /// DMA reads) wait for it until the next Drain.
    void NoteSync() {
        sync_pending = true;
    }
    [[nodiscard]] bool SyncPending() const {
        return sync_pending;
    }

    /// Makes what was sent visible to the back.
    void Publish();
    /// Waits until the back executed everything sent so far.
    void Drain(Nx2Profile::Zone zone = Nx2Profile::Zone::SplitDrain);
    /// The entry just sent can write guest memory (a compute, copy or upload engine's method):
    /// a command list on a GPU-written page waits for the back only up to here (DrainToLastWriter).
    void NoteWriter() {
        last_writer_position = write_position;
    }
    /// Waits until the back executed the last entry that could have written guest memory.
    void DrainToLastWriter();
    /// Waits until the back executed everything sent before ring position `position`.
    void DrainTo(u64 position);
    /// Ring positions (words sent, words the back finished, the last noted writer): what a
    /// command list on a GPU-written page would wait for (dma_pusher.cpp diagnostics).
    [[nodiscard]] u64 WritePosition() const { return write_position; }
    [[nodiscard]] u64 ConsumedPosition() const { return consumed.load(std::memory_order_acquire); }
    [[nodiscard]] u64 LastWriterPosition() const { return last_writer_position; }

private:
    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
        Regs,   ///< Bits 8-15 count; then per word method (low 32 bits) and argument (high 32)
        CbData, ///< As Regs, all constant-buffer data writes of the 3D engine
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = 256;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }

    static constexpr u32 MaxRegs = 64;
    void BlockWrite(Kind kind, Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        SelectEngine(engine);
        if (regs_entry != nullptr && regs_kind != kind) {
            CloseRegs();
        }
        if (regs_entry == nullptr) {
            regs_entry = Reserve(1 + MaxRegs);
            regs_kind = kind;
        }
        regs_entry[1 + regs_count] = (u64{argument} << 32) | method;
        if (++regs_count == MaxRegs) {
            CloseRegs();
        }
    }
    /// Commits the open Regs entry: before any other entry, a publish and a drain.
    void CloseRegs() {
        if (regs_entry == nullptr) {
            return;
        }
        regs_entry[0] = static_cast<u64>(regs_kind) | (u64{regs_count} << 8);
        regs_entry = nullptr;
        const u32 count = regs_count;
        regs_count = 0;
        Commit(1 + count);
    }
    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        if (regs_entry != nullptr) [[unlikely]] {
            CloseRegs();
        }
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        write_position += words;
        if (write_position - published_local >= publish_every) {
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);

    void BackLoop(std::stop_token stop_token);
    size_t Execute(const u64* entry);

    Core::System* system{};
    bool active{};
    bool front_macros{};
    bool reg_blocks{};
    std::unique_ptr<u64[]> ring;

    // Front side.
    u64 write_position{};
    u64 last_writer_position{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};
    /// The open Regs entry (reserved, not committed) and the writes in it.
    u64* regs_entry{};
    u32 regs_count{};
    Kind regs_kind{Kind::Regs};
    struct FrontRelease {
        u32 value;
        u64 position; ///< write_position after the release: run once consumed reaches it
    };
    std::unordered_map<u64, FrontRelease> front_releases;
    std::unordered_map<u64, u64> front_pending; ///< key -> write_position after the release
    std::unordered_map<u64, FrontRelease> front_lower_bounds;
    std::unordered_map<u64, std::vector<FrontRelease>> front_adds;
    std::unordered_map<u64, u64> front_reports; ///< key of each word -> write_position after it
    std::unordered_set<u64> reported_words;     ///< every word a report was sent to
    bool sync_pending{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};

    static GpuSplit instance;
    static inline thread_local bool back_thread_context{};
    void (*idle_hook)(void*){};
    void* idle_argument{};
    alignas(64) std::atomic<bool> hook_requested{};

    alignas(64) std::atomic<u64> published{};
    alignas(64) std::atomic<u64> consumed{};
    alignas(64) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    std::jthread back_thread;
};

} // namespace VideoCommon
