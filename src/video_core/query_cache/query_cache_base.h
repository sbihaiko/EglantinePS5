// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <mutex>
#include "video_core/query_cache/published_semaphore.h"
#include <optional>
#include <span>
#include "common/container/unordered_map.h"
#include <utility>

#include "common/assert.h"
#include "common/bit_field.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/query_cache/query_base.h"
#include "video_core/query_cache/region_visit.h"
#include "video_core/query_cache/types.h"

namespace VideoCore {
class RasterizerInterface;
}

namespace Tegra {
class GPU;
}

namespace VideoCommon {

struct LookupData {
    VAddr address;
    QueryBase* found_query;
};

template <typename Traits>
class QueryCacheBase : public VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
    using RuntimeType = typename Traits::RuntimeType;

public:
    union QueryLocation {
        BitField<27, 5, u32> stream_id;
        BitField<0, 27, u32> query_id;
        u32 raw;

        std::pair<size_t, size_t> unpack() const {
            return {static_cast<size_t>(stream_id.Value()), static_cast<size_t>(query_id.Value())};
        }
    };

    explicit QueryCacheBase(Tegra::GPU& gpu, VideoCore::RasterizerInterface& rasterizer_,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_,
                            RuntimeType& runtime_);

    ~QueryCacheBase();

    void InvalidateRegion(VAddr addr, std::size_t size) {
        published_semaphores.Invalidate(addr, size);
        IterateCache<true>(addr, size,
                           [this](QueryLocation location) { InvalidateQuery(location); });
    }

    void FlushRegion(VAddr addr, std::size_t size) {
        bool result = false;
        IterateCache<false>(addr, size, [this, &result](QueryLocation location) {
            result |= SemiFlushQueryDirty(location);
            return result;
        });
        if (result) {
            RequestGuestHostSync();
        }
    }

    static u64 BuildMask(std::span<const QueryType> types) {
        u64 mask = 0;
        for (auto query_type : types) {
            mask |= 1ULL << (static_cast<u64>(query_type));
        }
        return mask;
    }

    /// Return true when a CPU region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size) {
        bool result = false;
        IterateCache<false>(addr, size, [this, &result](QueryLocation location) {
            result |= IsQueryDirty(location);
            return result;
        });
        return result;
    }

    void CounterEnable(QueryType counter_type, bool is_enabled);

    void CounterReset(QueryType counter_type);

    void CounterClose(QueryType counter_type);

    void CounterReport(GPUVAddr addr, QueryType counter_type, QueryPropertiesFlags flags,
                       u32 payload, u32 subreport);

    void NotifyWFI();

    bool AccelerateHostConditionalRendering();

    /// The payload of the newest semaphore release to cpu_addr that the GPU processed but whose
    /// guest write still waits for the host GPU to finish the work before it (fences, and every
    /// release in accurate GPU mode).
    std::optional<u32> PendingRelease(DAddr cpu_addr);

    std::optional<u32> PublishedSemaphore(DAddr address, const void* pointer) {
        return published_semaphores.Read(address, pointer);
    }
    void InvalidatePublishedSemaphores(DAddr address, u64 size) {
        published_semaphores.Invalidate(address, size);
    }
    void ClearPublishedSemaphores() { published_semaphores.Clear(); }


    // Async downloads
    void CommitAsyncFlushes();

    bool HasUncommittedFlushes() const;

    /// Whether a new fence has queries to flush or host writes to sync for this cache.
    bool HasPendingFenceWork() const;

    bool ShouldWaitAsyncFlushes();

    void PopAsyncFlushes();

    void NotifySegment(bool resume);

    void BindToChannel(s32 id) override;

protected:
    template <bool remove_from_cache, typename Func>
    void IterateCache(VAddr addr, std::size_t size, Func&& func) {
        std::scoped_lock lock(cache_mutex);
        if (static u32 reports{}; size >= (u64{256} << 20) && reports++ < 16) {
            LOG_WARNING(HW_GPU, "MKW_QUERY_SPARSE span={} page_slots={} cache_pages={}", size,
                        size >> Core::DEVICE_PAGEBITS, cached_queries.size());
        }
        VisitQueryRegion<Core::DEVICE_PAGEBITS, remove_from_cache>(cached_queries, addr, size,
                                                                  std::forward<Func>(func));
    }

    using ContentCache = ::Common::unordered_map<u64, ::Common::unordered_map<u32, QueryLocation>>;

    void InvalidateQuery(QueryLocation location);
    bool IsQueryDirty(QueryLocation location);
    bool SemiFlushQueryDirty(QueryLocation location);
    void RequestGuestHostSync();
    void UnregisterPending();

    ::Common::unordered_map<u64, ::Common::unordered_map<u32, QueryLocation>> cached_queries;
    std::mutex cache_mutex;

    /// Releases not written to guest memory yet (PendingRelease). Written on the GPU thread,
    /// erased by the fence that writes them.
    ::Common::unordered_map<DAddr, u32> pending_releases;
    std::mutex pending_releases_mutex;
    PublishedSemaphoreStore<> published_semaphores;

    struct QueryCacheBaseImpl;
    friend struct QueryCacheBaseImpl;
    friend RuntimeType;

    std::unique_ptr<QueryCacheBaseImpl> impl;
};

} // namespace VideoCommon
