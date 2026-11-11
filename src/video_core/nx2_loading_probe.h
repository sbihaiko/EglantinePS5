// Temporary bounded loading-stall diagnostic; remove before retaining changes.
#pragma once
#include <atomic>
#include <array>
#include "common/common_types.h"
namespace VideoCommon::LoadingProbe {
struct Channel {
    std::atomic<const char*> phase{"none"};
    std::atomic<u64> argument{}, serial{};
    void Mark(const char* name, u64 value = 0) {
        argument.store(value, std::memory_order_relaxed);
        phase.store(name, std::memory_order_relaxed);
        serial.fetch_add(1, std::memory_order_relaxed);
    }
};
inline Channel gpu, compiler, driver, record, queue, wait, command, blit, writeback, enqueue;
inline thread_local bool registry_mutex_owned = false;
inline std::atomic<bool> enqueue_registry_owned{};
struct RegistryOwnerProbe {
    bool previous{registry_mutex_owned};
    RegistryOwnerProbe() { registry_mutex_owned = true; }
    ~RegistryOwnerProbe() { registry_mutex_owned = previous; }
};
inline thread_local bool software_writeback_active = false;
inline void MarkWriteback(const char* phase, u64 value = 0) {
    if (software_writeback_active) writeback.Mark(phase, value);
}
inline std::atomic<u64> blit_src{}, blit_dst{}, blit_lookup_loops{}, blit_rebind_loops{};
inline std::atomic<u64> command_address{}, command_word{}, command_raw{}, command_channel{},
    command_subchannel{}, command_remaining{}, command_value{};
struct GuestWait {
    std::atomic<u32> point{~0U};
    std::atomic<u64> target{}, requests{}, session{};
    std::atomic<bool> pending{};
};
inline std::array<GuestWait, 32> guest_waits{};
inline std::atomic<u64> sync_ro{}, sync_rw{}, sync_polls{}, sync_acks{};
inline std::atomic<u32> sync_count{};
inline std::atomic<bool> capture_enabled{false};
}
