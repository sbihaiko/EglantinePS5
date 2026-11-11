// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/rasterizer_download_area.h"

namespace VideoCommon::ReadbackDependencyDiagnostics {

inline u64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Window {
    static constexpr u64 Delay = 60'000'000'000;
    static constexpr u64 Duration = 30'000'000'000;
    static constexpr u64 Limit = 4096;
    std::atomic<u64> next_slot{};

    bool Select(u64 elapsed) {
        if (elapsed < Delay) {
            return false;
        }
        if (elapsed - Delay >= Duration) {
            next_slot.store(Limit, std::memory_order_relaxed);
            return false;
        }
        const u64 slot = (elapsed - Delay) * Limit / Duration;
        u64 next = next_slot.load(std::memory_order_relaxed);
        while (next <= slot) {
            if (next_slot.compare_exchange_weak(next, slot + 1, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }
};

struct Record {
    u64 core{}, address{}, size{}, fence{}, published_sequence{};
    u64 requested_generation{}, backend_generation{}, checked_generation{}, checked_sequence{};
    u64 return_generation{}, before_lock{}, enqueued{}, front_popped{}, backend_start{};
    u64 area_end{}, flush_end{}, cpu_return{};
    u32 docked_mode{}, gpu_accuracy{}, runtime_gpu_accuracy{}, dma_accuracy{}, cpu_accuracy{};
    bool preemptive_flag{}, split{};
    // Complete per-read frame capture is a separate opt-in from the sampled logger.
    u64 trace_index{~u64{0}}, request_ns{}, start_frame{}, end_frame{};
    u64 original_address{}, original_size{}, caller{}, dirty_mask{};
    u64 wait_begin{}, wait_end{}, completed_fence_at_request{}, pending_channels{};
    u64 scalar_valid{}, value_before{}, value_after{}, pending_before_valid{}, pending_before{};
    u64 pending_backend_valid{}, pending_backend{};
    u64 guest_pc{}, guest_lr{}, guest_sp{}, guest_x0{}, guest_x1{}, guest_x2{}, guest_x3{};
    u64 guest_x19{}, guest_x20{}, guest_x21{}, guest_x22{}, guest_x23{}, guest_x24{};
    u64 buffer_gpu_copy_bytes{}, buffer_cpu_copy_bytes{};
    u64 gpu_wait_ns{}, gpu_wait_count{}, gpu_wait_tick_max{};
};

// GPU-owner context: scopes nested backend work to this ordered read only.
inline thread_local Record* backend_record{};
class BackendScope {
public:
    explicit BackendScope(Record* record) : previous{backend_record} {
        backend_record = record && record->trace_index != ~u64{0} ? record : nullptr;
    }
    ~BackendScope() { backend_record = previous; }
private:
    Record* previous;
};
extern "C" void eden_buffer_readback_copy_bytes(u64, u64) __attribute__((weak));
inline void BufferCopyBytes(u64 gpu_bytes, u64 cpu_bytes) {
    if (eden_buffer_readback_copy_bytes) eden_buffer_readback_copy_bytes(gpu_bytes, cpu_bytes);
    if (backend_record) {
        backend_record->buffer_gpu_copy_bytes += gpu_bytes;
        backend_record->buffer_cpu_copy_bytes += cpu_bytes;
    }
}
extern "C" void eden_buffer_readback_source(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64)
    __attribute__((weak));
template <class Runtime, class Buffer>
inline void BufferCopySource(Runtime& runtime, const Buffer& buffer, u64 offset, u64 bytes, bool asynchronous) {
    if constexpr (requires { buffer.IsSparseCompatible(); buffer.Location().memory_type; runtime.CurrentTick(); }) {
        u64 identity = 0, producer = 0, version = 0, completed = 0;
        if constexpr (requires { buffer.ReadbackProducer(offset, bytes); buffer.ReadbackIdentity(); }) {
            if (const auto ticket = buffer.ReadbackProducer(offset, bytes)) {
                producer = ticket->producer;
                version = ticket->version;
                if constexpr (requires { runtime.IsFree(producer); }) completed = runtime.IsFree(producer);
            }
            identity = buffer.ReadbackIdentity();
        }
        if (eden_buffer_readback_source)
            eden_buffer_readback_source(buffer.CpuAddr(), buffer.SizeBytes(), offset, bytes,
                buffer.IsSparseCompatible(), buffer.Location().memory_type, asynchronous, runtime.CurrentTick(), identity, producer, version, completed);
    }
}

class GpuWaitScope {
public:
    explicit GpuWaitScope(u64 tick) : record{backend_record}, started{record ? Now() : 0} {
        if (record) {
            ++record->gpu_wait_count;
            if (tick > record->gpu_wait_tick_max) record->gpu_wait_tick_max = tick;
        }
    }
    ~GpuWaitScope() { if (record) record->gpu_wait_ns += Now() - started; }
private:
    Record* record;
    u64 started;
};

struct GuestContextProvider {
    void* object{};
    void (*capture)(void*, Record&){};
};
inline thread_local GuestContextProvider guest_context_provider;
class GuestContextScope {
public:
    GuestContextScope(void* object, void (*capture)(void*, Record&))
        : previous{guest_context_provider} { guest_context_provider = {object, capture}; }
    ~GuestContextScope() { guest_context_provider = previous; }
private:
    GuestContextProvider previous;
};

struct CallerContext { u64 kind{}, address{}, size{}; const void* pointer{}; };
inline thread_local CallerContext caller_context;
class CallerScope {
public:
    CallerScope(u64 kind, u64 address, u64 size, const void* pointer = nullptr) : previous{caller_context} {
        caller_context = {kind, address, size, pointer};
    }
    ~CallerScope() { caller_context = previous; }
private:
    CallerContext previous;
};

extern "C" bool eden_ordered_read_trace_begin(Record*) __attribute__((weak));
extern "C" void eden_ordered_read_trace_end(const Record*,
    const VideoCore::RasterizerDownloadArea*) __attribute__((weak));

// Observational snapshots only: they neither satisfy a guest load nor certify coherence.
inline u64 ScalarSnapshot(const CallerContext& context) {
    u64 value{};
    if (context.pointer && (context.size == 1 || context.size == 2 ||
                            context.size == 4 || context.size == 8))
        std::memcpy(&value, context.pointer, context.size);
    return value;
}

inline std::optional<Record> Begin() {
    Record trace;
    if (eden_ordered_read_trace_begin && eden_ordered_read_trace_begin(&trace)) {
        trace.caller = caller_context.kind;
        trace.original_address = caller_context.address;
        trace.original_size = caller_context.size;
        trace.scalar_valid = caller_context.kind == 4 && caller_context.pointer &&
            (caller_context.size == 1 || caller_context.size == 2 ||
             caller_context.size == 4 || caller_context.size == 8) &&
            (caller_context.address & 4095) + caller_context.size <= 4096;
        if (trace.scalar_valid) trace.value_before = ScalarSnapshot(caller_context);
        if (trace.scalar_valid && trace.original_size == 4 && guest_context_provider.capture)
            guest_context_provider.capture(guest_context_provider.object, trace);
        return trace;
    }
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_READBACK_DEPENDENCY_DIAGNOSTICS");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    if (!enabled) {
        return std::nullopt;
    }
    static const u64 origin = Now();
    static Window window;
    thread_local u64 requests{};
    if ((requests++ & 63) != 0) {
        return std::nullopt;
    }
    if (window.next_slot.load(std::memory_order_relaxed) == Window::Limit ||
        !window.Select(Now() - origin)) {
        return std::nullopt;
    }
    return Record{};
}

inline void Report(const Record& record, const VideoCore::RasterizerDownloadArea& result) {
    if (record.trace_index != ~u64{0}) {
        if (eden_ordered_read_trace_end) eden_ordered_read_trace_end(&record, &result);
        return; // Buffer binary records; never format/log each hot-path read.
    }
    LOG_INFO(HW_GPU,
             "ReadbackDependency core={} addr={:#x} size={} fence={} sequence={} split={} "
             "requested_generation={} backend_generation={} checked_generation={} "
             "checked_sequence={} completed_generation={} return_generation={} "
             "result_begin={:#x} result_end={:#x} preemptive_flag={} "
             "before_lock_ns={} enqueued_ns={} front_popped_ns={} backend_start_ns={} "
             "area_end_ns={} flush_end_ns={} cpu_return_ns={} docked_mode={} gpu_accuracy={} "
             "runtime_gpu_accuracy={} dma_accuracy={} cpu_accuracy={}",
             record.core, record.address, record.size, record.fence, record.published_sequence,
             record.split, record.requested_generation, record.backend_generation,
             record.checked_generation, record.checked_sequence, result.completed_generation,
             record.return_generation, result.start_address, result.end_address,
             record.preemptive_flag, record.before_lock, record.enqueued, record.front_popped,
             record.backend_start, record.area_end, record.flush_end, record.cpu_return,
             record.docked_mode, record.gpu_accuracy, record.runtime_gpu_accuracy,
             record.dma_accuracy, record.cpu_accuracy);
}

} // namespace VideoCommon::ReadbackDependencyDiagnostics
