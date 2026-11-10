// SPDX-License-Identifier: GPL-3.0-or-later
// Included once by performance.cpp. All file I/O runs outside the measured window.
#include "ordered_read_trace.h"
// The store reads the core's readback dependency records (core patch 0004). A core without them
// keeps the trace off: Frame counts, and nothing is recorded or exported.
#if __has_include("video_core/readback_dependency_diagnostics.h")
#include "ordered_read_trace_store.h"

#include <inttypes.h>
#include <memory>
#include <unistd.h>

namespace Eden::OrderedReadTrace {
using Capture = Store<65536, 241>;
static std::unique_ptr<Capture> capture_owner;
static std::atomic<Capture*> capture{};
static std::uint64_t last_nonce{};
static std::uint64_t last_polled_frame{};
static std::uint64_t pending_nonce{};
static unsigned pending_frames{};
static std::atomic<bool> semaphore_enabled{};
static std::atomic<bool> vulkan_handoff_enabled{};
// Diagnostic43: exercise range-local mapping updates during initial loading.
static std::atomic<bool> mapping_enabled{true};
static std::array<std::atomic<std::uint64_t>,2> mapping_operations{};
static std::atomic<std::uint64_t> semaphore_hits{};
static std::atomic<std::uint64_t> buffer_gpu_copy_bytes{}, buffer_cpu_copy_bytes{};
static std::atomic<unsigned> mapped_mode{1};
static std::array<std::atomic<std::uint64_t>,4> mapped_rejections{};
static std::atomic<std::uint64_t> mapped_direct_bytes{}, mapped_checked_bytes{}, mapped_failures{};
struct SemaphoreReasons {
    std::atomic<std::uint64_t> address{};
    std::array<std::atomic<std::uint64_t>, 7> counts{};
};
static std::array<SemaphoreReasons, 32> semaphore_reasons{};
static constexpr const char* arm_path = "/data/homebrew/PPSA98515/mkw-wait-arm.txt";

static void Export(Capture& c, const char* folder = "/data/eglantineps5/logs") {
    char prefix[192], path[224];
    std::snprintf(prefix, sizeof(prefix), "%s/mkw-waits-%d-%" PRIu64,
                  folder, getpid(), c.nonce);
    std::snprintf(path, sizeof(path), "%s-reads.csv", prefix);
    FILE* reads = std::fopen(path, "w");
    std::snprintf(path, sizeof(path), "%s-frames.csv", prefix);
    FILE* frames = std::fopen(path, "w");
    bool ok = reads && frames;
    if (reads) {
        std::setvbuf(reads, nullptr, _IOFBF, 128 * 1024);
        std::fputs("id,start_frame,end_frame,request_ns,cpu_return_ns,core,caller,original_address,original_size,address,size,fence,completed_fence_at_request,sequence,requested_generation,backend_generation,checked_generation,checked_sequence,completed_generation,return_generation,before_lock_ns,enqueued_ns,front_popped_ns,backend_start_ns,area_end_ns,flush_end_ns,wait_begin_ns,wait_end_ns,dirty_mask,pending_channels,split,result_begin,result_end,scalar_valid,value_before,value_after,pending_before_valid,pending_before,pending_backend_valid,pending_backend,guest_pc,guest_lr,guest_sp,guest_x0,guest_x1,guest_x2,guest_x3,guest_x19,guest_x20,guest_x21,guest_x22,guest_x23,guest_x24,buffer_gpu_copy_bytes,buffer_cpu_copy_bytes,gpu_wait_ns,gpu_wait_count,gpu_wait_tick_max\n", reads);
        for (std::size_t i = 0; i < c.Count(); ++i) {
            const auto& e = c.entries[i];
            const auto& r = e.read;
            const std::uint64_t values[]{r.trace_index,r.start_frame,r.end_frame,r.request_ns,
                r.cpu_return,r.core,r.caller,r.original_address,r.original_size,r.address,r.size,
                r.fence,r.completed_fence_at_request,r.published_sequence,r.requested_generation,
                r.backend_generation,r.checked_generation,r.checked_sequence,e.area.completed_generation,
                r.return_generation,r.before_lock,r.enqueued,r.front_popped,r.backend_start,
                r.area_end,r.flush_end,r.wait_begin,r.wait_end,r.dirty_mask,r.pending_channels,
                r.split,e.area.start_address,e.area.end_address,r.scalar_valid,r.value_before,r.value_after,
                r.pending_before_valid,r.pending_before,r.pending_backend_valid,r.pending_backend,
                r.guest_pc,r.guest_lr,r.guest_sp,r.guest_x0,r.guest_x1,r.guest_x2,r.guest_x3,
                r.guest_x19,r.guest_x20,r.guest_x21,r.guest_x22,r.guest_x23,r.guest_x24,
                r.buffer_gpu_copy_bytes,r.buffer_cpu_copy_bytes,r.gpu_wait_ns,
                r.gpu_wait_count,r.gpu_wait_tick_max};
            for (unsigned k = 0; k < std::size(values); ++k)
                std::fprintf(reads, k ? ",%" PRIu64 : "%" PRIu64, values[k]);
            std::fputc('\n', reads);
        }
        ok &= std::fclose(reads) == 0;
    }
    if (frames) {
        std::fputs("frame,boundary_ns,buffer_gpu_bytes,buffer_cpu_bytes,mapped_direct_bytes,mapped_checked_bytes,mapped_failures\n", frames);
        for (unsigned i = 0; i < c.frames; ++i)
            std::fprintf(frames, "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                c.boundaries[i].frame,c.boundaries[i].ns,
                c.boundaries[i].buffer_gpu_bytes,c.boundaries[i].buffer_cpu_bytes,
                c.boundaries[i].mapped_direct_bytes,c.boundaries[i].mapped_checked_bytes,
                c.boundaries[i].mapped_failures);
        ok &= std::fclose(frames) == 0;
    }
    std::snprintf(path, sizeof(path), "%s-copies.csv", prefix);
    bool copies_ok = false;
    if (FILE* output = std::fopen(path, "w")) {
        std::fputs("ns,frame,buffer_base,buffer_size,offset,size,sparse_compatible,memory_type,asynchronous,copy_tick,allocation_id,candidate_producer_tick,write_version,candidate_producer_completed\n", output);
        for (std::size_t i = 0; i < c.CopyCount(); ++i) {
            const auto& e = c.copies[i];
            const std::uint64_t values[]{e.ns,e.frame,e.base,e.buffer_size,e.offset,e.size,
                e.sparse_compatible,e.memory_type,e.asynchronous,e.copy_tick,e.allocation_id,e.candidate_producer_tick,e.write_version,e.candidate_producer_completed};
            for (unsigned k = 0; k < std::size(values); ++k)
                std::fprintf(output, k ? ",%" PRIu64 : "%" PRIu64, values[k]);
            std::fputc('\n', output);
        }
        copies_ok = std::fclose(output) == 0;
    }
    std::snprintf(path, sizeof(path), "%s-meta.txt", prefix);
    if (FILE* meta = std::fopen(path, "w")) {
        std::fprintf(meta, "version=4\nnonce=%" PRIu64 "\ncount=%zu\nattempted=%" PRIu64
                     "\ndropped=%" PRIu64 "\ninflight=%" PRIu64 "\nframes=%u\ncomplete=%d\n"
                     "caller0=unclassified\ncaller1=ReadBlock\ncaller2=CopyBlock\n"
                     "caller3=InvalidateDataCache\ncaller4=ScalarRead\n"
                     "dirty_mask=1:buffer,2:texture,4:query-at-backend-not-a-bypass-proof\n",
                     c.nonce,c.Count(),c.next.load(),c.dropped.load(),c.inflight.load(),c.frames-1,
                     int(ok && c.dropped.load()==0));
        std::fprintf(meta, "copy_sources=1\ncopy_count=%zu\ncopy_attempted=%" PRIu64
                     "\ncopy_dropped=%" PRIu64 "\ncopy_complete=%d\n",
                     c.CopyCount(), c.copy_next.load(), c.copy_dropped.load(),
                     int(copies_ok && c.copy_dropped.load() == 0));
        std::fclose(meta);
    }
    std::printf("MKW_WAIT_TRACE_DONE prefix=%s records=%zu dropped=%llu frames=%u complete=%d\n",
                prefix,c.Count(),static_cast<unsigned long long>(c.dropped.load()),c.frames-1,
                int(ok && c.dropped.load()==0));
    c.phase.store(Capture::Done, std::memory_order_release);
}

void Frame(std::uint64_t frame, std::uint64_t ns) {
    if (frame % 10 == 0) {
        const char* handoff_path = "/data/homebrew/PPSA98515/mkw-handoff-mode.txt";
        if (FILE* mode = std::fopen(handoff_path, "r")) {
            unsigned value{};
            if (std::fscanf(mode, "%u", &value) == 1 && value <= 1) {
                vulkan_handoff_enabled.store(value != 0, std::memory_order_release);
                std::printf("MKW_HANDOFF_MODE frame=%llu mode=%u\n",
                    static_cast<unsigned long long>(frame), value);
            }
            std::fclose(mode);
            std::remove(handoff_path);
        }

        const char* direct_path = "/data/homebrew/PPSA98515/mkw-mapped-mode.txt";
        if (FILE* mode = std::fopen(direct_path, "r")) {
            unsigned value{};
            if (std::fscanf(mode, "%u", &value) == 1 && value <= 2) mapped_mode.store(value);
            std::fclose(mode);
            std::remove(direct_path);
            std::printf("MKW_MAPPED_MODE frame=%llu mode=%u direct=%llu checked=%llu failures=%llu\n",
                static_cast<unsigned long long>(frame), mapped_mode.load(),
                static_cast<unsigned long long>(mapped_direct_bytes.load()),
                static_cast<unsigned long long>(mapped_checked_bytes.load()),
                static_cast<unsigned long long>(mapped_failures.load()));
        }
        if (frame % 120 == 0)
            std::printf("MKW_MAPPED_STATS frame=%llu mode=%u direct=%llu checked=%llu failures=%llu\n",
                static_cast<unsigned long long>(frame), mapped_mode.load(),
                static_cast<unsigned long long>(mapped_direct_bytes.load()),
                static_cast<unsigned long long>(mapped_checked_bytes.load()),
                static_cast<unsigned long long>(mapped_failures.load()));

        if (frame % 120 == 0)
            std::printf("MKW_MAPPED_REJECT frame=%llu unmapped=%llu opaque=%llu unknown=%llu pending=%llu\n",
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(mapped_rejections[0].load()),
                static_cast<unsigned long long>(mapped_rejections[1].load()),
                static_cast<unsigned long long>(mapped_rejections[2].load()),
                static_cast<unsigned long long>(mapped_rejections[3].load()));
        if (frame % 120 == 0)
            std::printf("MKW_MAPPING_STATE frame=%llu enabled=%u ops=%llu old_ops=%llu\n",
                static_cast<unsigned long long>(frame), unsigned(mapping_enabled.load()),
                static_cast<unsigned long long>(mapping_operations[1].load()),
                static_cast<unsigned long long>(mapping_operations[0].load()));
        const char* mapping_path = "/data/homebrew/PPSA98515/mkw-map-mode.txt";
        if (FILE* mode = std::fopen(mapping_path, "r")) {
            unsigned enabled{};
            if (std::fscanf(mode, "%u", &enabled) == 1 && enabled <= 1) {
                mapping_enabled.store(enabled != 0, std::memory_order_release);
                std::printf("MKW_MAPPING_MODE frame=%llu enabled=%u ops=%llu old_ops=%llu\n",
                    static_cast<unsigned long long>(frame), enabled,
                    static_cast<unsigned long long>(mapping_operations[1].load()),
                    static_cast<unsigned long long>(mapping_operations[0].load()));
            }
            std::fclose(mode);
            std::remove(mapping_path);
        }
        const char* mode_path = "/data/homebrew/PPSA98515/mkw-sem-mode.txt";
        if (FILE* mode = std::fopen(mode_path, "r")) {
            unsigned enabled{};
            if (std::fscanf(mode, "%u", &enabled) == 1 && enabled <= 1) {
                semaphore_enabled.store(enabled != 0, std::memory_order_release);
                std::printf("MKW_SEMAPHORE_MODE frame=%llu enabled=%u hits=%llu\n",
                    static_cast<unsigned long long>(frame), enabled,
                    static_cast<unsigned long long>(semaphore_hits.load()));
            }
            std::fclose(mode);
            std::remove(mode_path);
        }
    }
    if (frame % 30 == 0)
        std::printf("MKW_SEMAPHORE_STATE frame=%llu enabled=%u hits=%llu\n",
            static_cast<unsigned long long>(frame), unsigned(semaphore_enabled.load()),
            static_cast<unsigned long long>(semaphore_hits.load()));
    if (frame % 30 == 0 && semaphore_enabled.load()) {
        for (const auto& entry : semaphore_reasons) {
            const auto address = entry.address.load(std::memory_order_acquire);
            if (!address) continue;
            std::printf("MKW_SEMAPHORE_REASONS frame=%llu addr=0x%llx ok=%llu buffer_busy=%llu texture_busy=%llu buffer_dirty=%llu texture_dirty=%llu unpublished=%llu value_changed=%llu\n",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(entry.counts[0].load()),
                static_cast<unsigned long long>(entry.counts[1].load()),
                static_cast<unsigned long long>(entry.counts[2].load()),
                static_cast<unsigned long long>(entry.counts[3].load()),
                static_cast<unsigned long long>(entry.counts[4].load()),
                static_cast<unsigned long long>(entry.counts[5].load()),
                static_cast<unsigned long long>(entry.counts[6].load()));
        }
    }
    auto* c = capture.load(std::memory_order_acquire);
    if (pending_nonce) {
        // Allocate before this boundary: arming must not count an unobserved
        // allocation interval as the first measured frame.
        c = capture_owner.get();
        c->Start(pending_nonce, frame, ns, pending_frames,
                 buffer_gpu_copy_bytes.load(), buffer_cpu_copy_bytes.load(),
                 mapped_direct_bytes.load(), mapped_checked_bytes.load(), mapped_failures.load());
        capture.store(c, std::memory_order_release);
        std::printf("MKW_WAIT_TRACE_ARM nonce=%llu first_boundary=%llu frames=%u capacity=65536\n",
                    static_cast<unsigned long long>(pending_nonce),
                    static_cast<unsigned long long>(frame), pending_frames);
        pending_nonce = 0;
        return;
    }
    if (c) {
        c->Frame(frame, ns, buffer_gpu_copy_bytes.load(), buffer_cpu_copy_bytes.load(),
                 mapped_direct_bytes.load(), mapped_checked_bytes.load(), mapped_failures.load());
        if (c->Ready()) {
            c->phase.store(Capture::Exporting, std::memory_order_release);
            std::thread([c] { Export(*c); }).detach();
        }
        if (c->phase.load(std::memory_order_acquire) != Capture::Done) return;
    }
    if (frame < last_polled_frame + 10) return;
    last_polled_frame = frame;
    FILE* arm = std::fopen(arm_path, "r");
    if (!arm) return;
    unsigned long long nonce{};
    unsigned count{};
    const bool valid = std::fscanf(arm, "%llu %u", &nonce, &count) == 2 &&
                       nonce && nonce != last_nonce && count && count <= 240;
    std::fclose(arm);
    if (!valid) return;
    if (!capture_owner) capture_owner = std::make_unique<Capture>();
    pending_nonce = nonce;
    pending_frames = count;
    last_nonce = nonce;
    std::remove(arm_path);
}
}

extern "C" bool eden_ordered_read_trace_begin(VideoCommon::ReadbackDependencyDiagnostics::Record* r) {
    auto* c = Eden::OrderedReadTrace::capture.load(std::memory_order_acquire);
    if (!c || c->phase.load(std::memory_order_acquire) != Eden::OrderedReadTrace::Capture::Recording)
        return false;
    return c->Begin(*r, VideoCommon::ReadbackDependencyDiagnostics::Now());
}
extern "C" void eden_ordered_read_trace_end(
    const VideoCommon::ReadbackDependencyDiagnostics::Record* r,
    const VideoCore::RasterizerDownloadArea* area) {
    auto* c = Eden::OrderedReadTrace::capture.load(std::memory_order_acquire);
    if (c) c->End(*r, *area);
}

extern "C" bool eden_published_semaphore_reads_enabled() {
    return Eden::OrderedReadTrace::semaphore_enabled.load(std::memory_order_acquire);
}
extern "C" void eden_published_semaphore_read_hit() {
    Eden::OrderedReadTrace::semaphore_hits.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void eden_published_semaphore_read_reason(std::uint64_t address, unsigned reason) {
    if (!address || reason >= 7) return;
    for (auto& entry : Eden::OrderedReadTrace::semaphore_reasons) {
        auto key = entry.address.load(std::memory_order_relaxed);
        if (!key && entry.address.compare_exchange_strong(key, address)) key = address;
        if (key != address) continue;
        entry.counts[reason].fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

extern "C" bool eden_mapping_window_enabled() {
    const bool enabled = Eden::OrderedReadTrace::mapping_enabled.load(std::memory_order_acquire);
    Eden::OrderedReadTrace::mapping_operations[enabled].fetch_add(1, std::memory_order_relaxed);
    return enabled;
}

extern "C" void eden_buffer_readback_copy_bytes(std::uint64_t gpu, std::uint64_t cpu) {
    Eden::OrderedReadTrace::buffer_gpu_copy_bytes.fetch_add(gpu, std::memory_order_relaxed);
    Eden::OrderedReadTrace::buffer_cpu_copy_bytes.fetch_add(cpu, std::memory_order_relaxed);
}

extern "C" void eden_buffer_readback_source(std::uint64_t base, std::uint64_t buffer_size,
    std::uint64_t offset, std::uint64_t size, std::uint64_t sparse, std::uint64_t type,
    std::uint64_t asynchronous, std::uint64_t copy_tick, std::uint64_t allocation_id,
    std::uint64_t candidate_producer_tick, std::uint64_t write_version,
    std::uint64_t candidate_producer_completed) {
    auto* c = Eden::OrderedReadTrace::capture.load(std::memory_order_acquire);
    if (!c || c->phase.load(std::memory_order_acquire) != Eden::OrderedReadTrace::Capture::Recording)
        return;
    c->Copy({VideoCommon::ReadbackDependencyDiagnostics::Now(), 0, base, buffer_size,
             offset, size, sparse, type, asynchronous, copy_tick, allocation_id, candidate_producer_tick, write_version, candidate_producer_completed});
}

extern "C" unsigned eden_mapped_readback_mode() {
    return Eden::OrderedReadTrace::mapped_mode.load(std::memory_order_acquire);
}
extern "C" void eden_mapped_readback_result(std::uint64_t direct, std::uint64_t checked, std::uint64_t failures) {
    Eden::OrderedReadTrace::mapped_direct_bytes.fetch_add(direct);
    Eden::OrderedReadTrace::mapped_checked_bytes.fetch_add(checked);
    Eden::OrderedReadTrace::mapped_failures.fetch_add(failures);
}

extern "C" void eden_mapped_readback_reject(unsigned reason) {
    if (reason < 4) Eden::OrderedReadTrace::mapped_rejections[reason].fetch_add(1);
}

extern "C" bool eden_vulkan_handoff_enabled() {
    return Eden::OrderedReadTrace::vulkan_handoff_enabled.load(std::memory_order_acquire);
}
#else
namespace Eden::OrderedReadTrace {
void Frame(std::uint64_t, std::uint64_t) {}
} // namespace Eden::OrderedReadTrace
#endif
