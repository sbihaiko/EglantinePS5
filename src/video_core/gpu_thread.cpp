#include "video_core/nx2_loading_probe.h"
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <thread>

#include "common/assert.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_profile.h"
#include "video_core/readback_diagnostics.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GPUThread {

namespace {

VideoCore::RasterizerDownloadArea ReadMemory(VideoCore::RasterizerInterface& rasterizer,
                                           const ThreadManager& manager, const SynchState& state,
                                           DAddr addr, u64 size, u64 published_work_sequence,
                                           ReadbackDependencyDiagnostics::Record* diagnostic) {
    if (diagnostic) {
        diagnostic->backend_start = ReadbackDependencyDiagnostics::Now();
    }
    ReadbackDependencyDiagnostics::BackendScope backend_scope{diagnostic};
    const u64 generation = manager.ReadbackGeneration();
    if (diagnostic && diagnostic->trace_index != ~u64{0}) {
        using VideoCommon::CacheType;
        // Snapshot on the GPU owner, after preceding work. A zero mask here alone is
        // NOT permission to bypass ordering at the CPU request time.
        const auto scalar_address = addr + (size == 4096 ? diagnostic->original_address & 4095 : 0);
        if (diagnostic->scalar_valid) {
            const auto pending = rasterizer.PendingSemaphoreRelease(scalar_address);
            diagnostic->pending_backend_valid = pending.has_value();
            diagnostic->pending_backend = pending.value_or(0);
        }
        diagnostic->dirty_mask =
            (rasterizer.MustFlushRegion(addr, size, CacheType::BufferCache) ? 1 : 0) |
            (rasterizer.MustFlushRegion(addr, size, CacheType::TextureCache) ? 2 : 0) |
            (rasterizer.MustFlushRegion(addr, size, CacheType::QueryCache) ? 4 : 0);
    }
    auto area = rasterizer.GetFlushArea(addr, size);
    if (diagnostic) {
        diagnostic->backend_generation = generation;
        diagnostic->area_end = ReadbackDependencyDiagnostics::Now();
        diagnostic->preemptive_flag = area.preemtive;
    }
    rasterizer.FlushRegion(area.start_address, area.end_address - area.start_address);
    area.preemtive = true;
    if (diagnostic) {
        diagnostic->flush_end = ReadbackDependencyDiagnostics::Now();
        diagnostic->checked_generation = manager.ReadbackGeneration();
        diagnostic->checked_sequence = state.published_work_sequence.load(std::memory_order_acquire);
        area.completed_generation =
            generation != 0 && diagnostic->checked_generation == generation &&
                    diagnostic->checked_sequence == published_work_sequence
                ? generation
                : 0;
    } else {
        area.completed_generation =
            generation != 0 && manager.ReadbackGeneration() == generation &&
                    state.published_work_sequence.load(std::memory_order_acquire) == published_work_sequence
                ? generation
                : 0;
    }
    return area;
}

// Tells a CPU thread waiting in PushCommand that its command ran.
void SignalCommand(SynchState& state, u64 fence,
                   const std::shared_ptr<Common::HostCommandCompletion>& completion) {
    state.signaled_fence.store(fence, std::memory_order_release);
    if (completion) completion->Signal();
}

} // Anonymous namespace

ThreadManager::ThreadManager(Core::System& system_)
    : system{system_}
{}

ThreadManager::~ThreadManager() = default;

void ThreadManager::StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    rasterizer = renderer.ReadRasterizer();
    thread = std::jthread([&](std::stop_token stop_token) {
        Common::SetCurrentThreadName("GPU");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        Common::SetCurrentThreadToPerformanceCores();
        system.RegisterHostThread();

        auto current_context = context.Acquire();
        CommandDataContainer next;
        // When a pending channel last moved or a command arrived (see the polling below).
        auto last_progress = std::chrono::steady_clock::now();
        namespace Profile = VideoCommon::Nx2Profile;
        auto& split = VideoCommon::GpuSplit::Get();
        while (true) {
            next = {};
            Profile::Report();
            split.MaybeStart(system);
            if (stop_token.stop_requested()) {
                if (!state.queue.TryPop(next)) {
                    break;
                }
            } else if (scheduler.HasPendingChannels()) {
                // Suspended channels wait on semaphores the CPU releases: poll them meanwhile.
                {
                    Profile::Scope scope{Profile::Zone::ResumePending};
                    scheduler.ResumePending(system.GPU());
                }
                if (!state.queue.TryPop(next)) {
                    // The CPU usually releases the semaphore soon after: poll for half a
                    // millisecond before sleeping, or every wait also waits out a host sleep
                    // (title 1 waits ~25 times a second).
                    Profile::Scope scope{Profile::Zone::PendingSleep};
                    if (std::chrono::steady_clock::now() - last_progress <
                        std::chrono::microseconds{500}) {
                        std::this_thread::yield();
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds{1});
                    }
                    continue;
                }
            } else {
                Profile::Scope scope{Profile::Zone::Idle};
                state.queue.PopWait(next, stop_token);
            }
            if (std::holds_alternative<std::monostate>(next.data)) {
                continue;
            }
            if (const auto* read = std::get_if<CpuReadCommand>(&next.data);
                read && read->diagnostic) {
                read->diagnostic->front_popped = ReadbackDependencyDiagnostics::Now();
                read->diagnostic->split = split.Active();
                read->diagnostic->pending_channels = scheduler.HasPendingChannels();
            }
            last_progress = std::chrono::steady_clock::now();
            if (split.Active()) {
                // Only command lists are parsed here; everything else runs on the back thread
                // in order, and so does the signal that the command ran.
                auto* const rasterizer_ptr = renderer.ReadRasterizer();
                if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                    Profile::Scope scope{Profile::Zone::Push};
                    // Held-back guest CPU writes first (see the single-thread path below).
                    split.Defer([gpu = &system.GPU()] { gpu->InvalidateGPUCache(); });
                    scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
                } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                    // TickWork ends the fast draw delta chain itself when it runs a request.
                    split.Defer([gpu = &system.GPU()] { gpu->TickWork(); },
                                VideoCommon::GpuSplit::FastDrawNeutral);
                } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                    split.DeferFunction(std::move(run->func));
                } else if (const auto* read = std::get_if<CpuReadCommand>(&next.data)) {
                    split.Defer([this, rasterizer_ptr, command = *read,
                                 sequence = next.published_work_sequence] {
                        *command.result = GPUThread::ReadMemory(*rasterizer_ptr, *this, state,
                                                              command.addr, command.size, sequence,
                                                              command.diagnostic);
                    });
                } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = flush->addr, size = flush->size] {
                        rasterizer_ptr->FlushRegion(addr, size);
                    });
                } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                    split.Defer([rasterizer_ptr, addr = invalidate->addr, size = invalidate->size] {
                        rasterizer_ptr->OnCacheInvalidation(addr, size);
                    });
                } else {
                    ASSERT(false);
                }
                split.DeferFunction([state_ptr = &state, fence = next.fence, completion = next.completion] {
                    SignalCommand(*state_ptr, fence, completion);
                }, VideoCommon::GpuSplit::FastDrawNeutral);
                split.Publish();
                continue;
            }
            if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Push};
                // Guest CPU writes to GPU-written memory are held back until the GPU thread
                // applies them, by default at the next fence. The guest made them before it
                // submitted this list, which may read them: apply them first. Pokemon Pokopia
                // keeps constants in pages its native storage bindings mark GPU-written, and some
                // frames drew stale ones (flickering shadows, stretched geometry).
                system.GPU().InvalidateGPUCache();
                scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
            } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                Profile::Scope scope{Profile::Zone::Tick};
                system.GPU().TickWork();
            } else if (auto* run = std::get_if<RunCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::Run};
                run->func();
            } else if (const auto* read = std::get_if<CpuReadCommand>(&next.data)) {
                *read->result = GPUThread::ReadMemory(*renderer.ReadRasterizer(), *this, state,
                                                    read->addr, read->size,
                                                    next.published_work_sequence, read->diagnostic);
            } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->FlushRegion(flush->addr, flush->size);
            } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                Profile::Scope scope{Profile::Zone::FlushInvalidate};
                renderer.ReadRasterizer()->OnCacheInvalidation(invalidate->addr, invalidate->size);
            } else {
                ASSERT(false);
            }
            SignalCommand(state, next.fence, next.completion);
        }
        if (split.Active()) {
            split.Drain();
        }
    });
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList&& entries, bool is_async) {
    PushCommand(SubmitListCommand(channel, std::move(entries)), false, is_async);
}

void ThreadManager::FlushRegion(DAddr addr, u64 size, bool is_async) {
    if (!is_async) {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size), false, is_async);
    }
}

void ThreadManager::TickGPU(bool is_async) {
    PushCommand(GPUTickCommand(), false, is_async);
}

VideoCore::RasterizerDownloadArea ThreadManager::ReadMemory(DAddr addr, u64 size, bool is_async) {
    ReadbackDiagnostics::Scope diagnostic{ReadbackDiagnostics::Event::OrderedRead, size};
    VideoCore::RasterizerDownloadArea result{};
    auto dependency = ReadbackDependencyDiagnostics::Begin();
    if (dependency) {
        dependency->core = system.GetCurrentHostThreadID();
        dependency->address = addr;
        dependency->size = size;
        dependency->requested_generation = ReadbackGeneration();
        dependency->completed_fence_at_request = state.signaled_fence.load(std::memory_order_acquire);
        dependency->docked_mode = static_cast<u32>(Settings::values.use_docked_mode.GetValue());
        dependency->gpu_accuracy = static_cast<u32>(Settings::values.gpu_accuracy.GetValue());
        dependency->runtime_gpu_accuracy = static_cast<u32>(Settings::values.current_gpu_accuracy);
        dependency->dma_accuracy = static_cast<u32>(Settings::values.dma_accuracy.GetValue());
        dependency->cpu_accuracy = static_cast<u32>(Settings::values.cpu_accuracy.GetValue());
    }
    if (dependency && dependency->scalar_valid) {
        const auto scalar_address = addr + (size == 4096 ? dependency->original_address & 4095 : 0);
        const auto pending = rasterizer->PendingSemaphoreRelease(scalar_address);
        dependency->pending_before_valid = pending.has_value();
        dependency->pending_before = pending.value_or(0);
    }
    PushCommand(CpuReadCommand{addr, size, &result, dependency ? &*dependency : nullptr}, true, is_async);
    if (dependency) {
        if (dependency->scalar_valid)
            dependency->value_after = ReadbackDependencyDiagnostics::ScalarSnapshot(
                ReadbackDependencyDiagnostics::caller_context);
        dependency->cpu_return = ReadbackDependencyDiagnostics::Now();
        dependency->return_generation = ReadbackGeneration();
        ReadbackDependencyDiagnostics::Report(*dependency, result);
    }
    return result;
}

u64 ThreadManager::ReadbackGeneration() const {
    if (state.async_guest_writes.load(std::memory_order_acquire) != 0) {
        return 0;
    }
    const u64 generation = state.readback_generation.load(std::memory_order_acquire);
    if (state.async_guest_writes.load(std::memory_order_acquire) != 0) {
        return 0;
    }
    return generation;
}

void ThreadManager::InvalidateReadbackCache() {
    state.readback_generation.fetch_add(1, std::memory_order_acq_rel);
}

void ThreadManager::BeginAsyncGuestWrite() {
    state.async_guest_writes.fetch_add(1, std::memory_order_acq_rel);
    InvalidateReadbackCache();
}

void ThreadManager::EndAsyncGuestWrite() {
    InvalidateReadbackCache();
    state.async_guest_writes.fetch_sub(1, std::memory_order_acq_rel);
}

void ThreadManager::Run(std::function<void()>&& func, bool is_async) {
    PushCommand(RunCommand{std::move(func)}, false, is_async);
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size) {
    BeginAsyncGuestWrite();
    SCOPE_EXIT { EndAsyncGuestWrite(); };
    rasterizer->OnCacheInvalidation(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size, bool is_async) {
    BeginAsyncGuestWrite();
    SCOPE_EXIT { EndAsyncGuestWrite(); };
    if (Settings::IsGPULevelHigh()) {
        if (!is_async) {
            PushCommand(FlushRegionCommand(addr, size), false, is_async);
        } else {
            auto& gpu = system.GPU();
            const u64 fence = gpu.RequestFlush(addr, size);
            TickGPU(is_async);
            gpu.WaitForSyncOperation(fence);
        }
    }
    rasterizer->OnCacheInvalidation(addr, size);
}

u64 ThreadManager::PushCommand(CommandData&& command_data, bool block, bool is_async) {
    if (!is_async) {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    auto* read_command = std::get_if<CpuReadCommand>(&command_data);
    auto* read_diagnostic = read_command ? read_command->diagnostic : nullptr;
    if (read_diagnostic) read_diagnostic->before_lock = ReadbackDependencyDiagnostics::Now();
    // Reuse one completion per synchronous caller. Each queue item owns a reference
    // until its notification returns; no allocation per read after the first call.
    auto completion = block ? Common::HostCommandCompletion::ForCurrentThread() : nullptr;
    std::unique_lock lk(state.write_lock);
    if (state.shutting_down) {
        return 0;
    }
    if (!std::holds_alternative<CpuReadCommand>(command_data)) {
        state.published_work_sequence.fetch_add(1, std::memory_order_acq_rel);
        InvalidateReadbackCache();
    }
    const u64 fence{++state.last_fence};
    state.queue.Emplace(std::move(command_data), fence, completion,
                        state.published_work_sequence.load(std::memory_order_acquire));

    lk.unlock();
    if (read_diagnostic) read_diagnostic->wait_begin = ReadbackDependencyDiagnostics::Now();
    if (completion) completion->Wait();
    if (read_diagnostic) read_diagnostic->wait_end = ReadbackDependencyDiagnostics::Now();

    return fence;
}

void ThreadManager::NotifyShutdown() {
    if (thread.joinable()) {
        {
            std::scoped_lock lk{state.write_lock};
            state.shutting_down = true;
            InvalidateReadbackCache();
            thread.request_stop();
        }
        thread.join();
    }
    VideoCommon::GpuSplit::Get().Stop();
}

} // namespace VideoCommon::GPUThread
