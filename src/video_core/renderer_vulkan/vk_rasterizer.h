// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <unordered_map>

#include <boost/container/static_vector.hpp>

#include "common/common_types.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_fence_manager.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core {
class System;
}

namespace Core::Frontend {
class EmuWindow;
}

namespace Tegra {

namespace Engines {
class Maxwell3D;
}

} // namespace Tegra

namespace Vulkan {

struct FramebufferTextureInfo;

class StateTracker;

class AccelerateDMA : public Tegra::Engines::AccelerateDMAInterface {
public:
    explicit AccelerateDMA(BufferCache& buffer_cache, TextureCache& texture_cache,
                           Scheduler& scheduler);

    bool BufferCopy(GPUVAddr start_address, GPUVAddr end_address, u64 amount) override;

    bool BufferClear(GPUVAddr src_address, u64 amount, u32 value) override;

    bool ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::ImageOperand& src,
                       const Tegra::DMA::BufferOperand& dst) override;

    bool BufferToImage(const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& src,
                       const Tegra::DMA::ImageOperand& dst) override;

private:
    template <bool IS_IMAGE_UPLOAD>
    bool DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                            const Tegra::DMA::BufferOperand& src,
                            const Tegra::DMA::ImageOperand& dst);

    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    Scheduler& scheduler;
};

class RasterizerVulkan final : public VideoCore::RasterizerInterface,
                               protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    explicit RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                              Tegra::MaxwellDeviceMemoryManager& device_memory_,
                              const Device& device_, MemoryAllocator& memory_allocator_,
                              StateTracker& state_tracker_, Scheduler& scheduler_);
    ~RasterizerVulkan() override;

    void Draw(bool is_indexed, u32 instance_count) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override;
    std::optional<u32> PendingSemaphoreRelease(DAddr addr) override;
    std::optional<u32> ReadPublishedSemaphore(DAddr addr, const void* pointer) override;
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    void FlushAll() override;
    void FlushRegion(DAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override;
    void InvalidateRegion(DAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override;
    void OnCacheInvalidation(DAddr addr, u64 size) override;
    void OnCachedWrite(DAddr addr, u64 size) override;
    bool TryCacheInvalidation(DAddr addr, u64 size) override;
    bool OnCPUWrite(DAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override;
    void SignalFence(std::function<void()>&& func) override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void SignalReference() override;
    void ReleaseFences(bool force = true) override;
    void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;
    void TickWithoutPresent() override;
    bool AccelerateConditionalRendering() override;
    bool HasDrawTransformFeedback() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                               const Tegra::Engines::Fermi2D::Surface& dst,
                               const Tegra::Engines::Fermi2D::Config& copy_config) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) override;

    void InitializeChannel(Tegra::Control::ChannelState& channel) override;

    void BindChannel(Tegra::Control::ChannelState& channel) override;

    void ReleaseChannel(s32 channel_id) override;
    std::optional<FramebufferTextureInfo> AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                                            VAddr framebuffer_addr,
                                                            u32 pixel_stride);

private:
    static constexpr const u64 NEEDS_D24[] = {
        0x01006A800016E000ULL, // SSBU
        0x0100E95004038000ULL, // XC2
        0x0100A6301214E000ULL, // FE:Engage
    };
    static constexpr size_t MAX_TEXTURES = 192;
    static constexpr size_t MAX_IMAGES = 48;
    static constexpr size_t MAX_IMAGE_VIEWS = MAX_TEXTURES + MAX_IMAGES;

    static constexpr VkDeviceSize DEFAULT_BUFFER_SIZE = 4 * sizeof(float);

    template <typename Func>
    void PrepareDraw(bool is_indexed, Func&&);

    void FlushWork();

    void UpdateDynamicStates();

    void HandleTransformFeedback();

    void UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateScissorsState(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBias(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateBlendConstants(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBounds(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilFaces(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineWidth(Tegra::Engines::Maxwell3D::Regs& regs);

    void UpdateCullMode(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBoundsTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthCompareOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdatePrimitiveRestartEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateRasterizerDiscardEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateConservativeRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineStippleEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineStipple(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLineRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLogicOpEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateDepthClampEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateAlphaToCoverageEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateAlphaToOneEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateFrontFace(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateStencilTestEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateLogicOp(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateBlending(Tegra::Engines::Maxwell3D::Regs& regs);
    void UpdateColorWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs);
    /// Fragment output components (4 bits per render target) of the draw being prepared, and those
    /// the dynamic color write mask was last recorded with
    u32 draw_color_output_mask{~0U};
    u32 last_color_output_mask{~0U};

    void UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs);

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;

    const Device& device;
    MemoryAllocator& memory_allocator;
    StateTracker& state_tracker;
    Scheduler& scheduler;

    StagingBufferPool staging_pool;
    DescriptorPool descriptor_pool;
    GuestDescriptorQueue guest_descriptor_queue;
    ComputePassDescriptorQueue compute_pass_descriptor_queue;
    DescriptorBufferRing descriptor_buffer_ring;
    BlitImageHelper blit_image;
    RenderPassCache render_pass_cache;

    TextureCacheRuntime texture_cache_runtime;
    TextureCache texture_cache;
    BufferCacheRuntime buffer_cache_runtime;
    BufferCache buffer_cache;
    QueryCacheRuntime query_cache_runtime;
    QueryCache query_cache;
    PipelineCache pipeline_cache;
    AccelerateDMA accelerate_dma;
    FenceManager fence_manager;

    vk::Event wfi_event;

    boost::container::static_vector<u32, MAX_IMAGE_VIEWS> image_view_indices;
    std::array<VideoCommon::ImageViewId, MAX_IMAGE_VIEWS> image_view_ids;
    boost::container::static_vector<VkSampler, MAX_TEXTURES> sampler_handles;

    u32 draw_counter = 0;
    std::chrono::steady_clock::time_point last_cache_tick = std::chrono::steady_clock::now();
    u64 idle_tick_gpu{}; ///< GPU tick that must complete before the next idle cache tick
    bool mesh_draw{}; ///< the draw being prepared uses a task/mesh pipeline

    using FastDrawMaxwell = Tegra::Engines::Maxwell3D::Regs;
    /// Fast draw (nx2_fast_draw.h): what the last recorded draw left bound.
    struct ReusableDraw {
        GraphicsPipeline* pipeline{};
        const Tegra::Engines::Maxwell3D* maxwell3d{};
        u64 serial{};
        u64 map_serial{};
        u64 tick{};
        u64 render_pass{};
        FastDrawMaxwell::PrimitiveTopology topology{};
        bool is_indexed{};
    };
    enum class FullDraw : u8 {
        NoCandidate,
        Kind,
        StateChanged,
        Submitted,
        RenderPass,
        DeltaPipeline,
        HandleBind,
        HandleData,
        SampledImages,
        Count,
    };
    /// Why a recorded draw can't be followed by a delta draw.
    enum class Ineligible : u8 {
        Indirect,
        Mesh,
        NotBuilt,
        WritesImages,
        TransformFeedback,
        InlineIndex,
        Quads,
        NoRenderPass,
        PipelineNotBound,
        OffBackThread,
        Count,
    };
    enum class DrawPath : u8 { Full, Delta, Count };
    struct FastDrawStats {
        u64 delta{};
        std::array<u64, static_cast<size_t>(FullDraw::Count)> full{};
        std::array<u64, static_cast<size_t>(Ineligible::Count)> ineligible{};
        /// PrepareDraw time and draws per path (timed with EDEN_NX2_GPU_PROFILE or verify only).
        std::array<u64, static_cast<size_t>(DrawPath::Count)> prepare_ns{};
        std::array<u64, static_cast<size_t>(DrawPath::Count)> prepare_draws{};
        /// EDEN_NX2_FAST_DRAW_VERIFY
        u64 verified{};
        u64 pipeline_mismatches{};
        u64 view_mismatches{};
        u64 binding_mismatches{};
        /// DeltaPipeline draws by GraphicsPipeline::DeltaDrawBlocker.
        std::array<u64, static_cast<size_t>(GraphicsPipeline::DeltaBlocker::Count)> delta_blockers{};
        /// StateChanged draws by the reason of the last break, and by its 3D method.
        std::array<u64, static_cast<size_t>(VideoCommon::Nx2FastDraw::Reason::Count)> breaks{};
        std::array<u32, FastDrawMaxwell::NUM_REGS> break_methods{};
        /// EDEN_NX2_COUNTERS: StateChanged draws by the set of breaks since the previous draw.
        /// Sizes: none on this thread (breaks on another thread), 1, 2, 3, 4-8, more than 8.
        std::array<u64, 6> set_sizes{};
        std::unordered_map<u32, u32> sole_sources; ///< Sets of one break: which
        std::unordered_map<u32, u32> any_sources;  ///< Draws whose set holds the break
        std::unordered_map<u64, u32> pair_sources; ///< Sets of two breaks
        u64 probe_only{}; ///< Sets made only of EDEN_NX2_FAST_DRAW_PROBE methods
        u64 window_start{};
    };
    /// The pipeline the draw can take the delta path with, or null for a full resolve. Clears
    /// the remembered draw either way: only a recorded draw remembers one again.
    GraphicsPipeline* TakeReusableDraw(bool is_indexed, bool is_indirect);
    /// Whether the draw TakeReusableDraw took still holds once the caches are locked (serial and
    /// map_serial as read then), and the images it samples need no preparing (Lichii #921).
    bool DeltaStillHolds(u64 serial, u64 map_serial);
    void NoteResolvedDraw(GraphicsPipeline* pipeline, bool is_indexed, bool is_indirect, u64 serial,
                          u64 map_serial);
    void ReportFastDraws();
    u32 fast_draw_report_ticks{};
    void CountBreakSet(const VideoCommon::Nx2FastDraw::BreakSet& breaks);
    ReusableDraw reusable_draw;
    ReusableDraw taken_draw; ///< The draw TakeReusableDraw last returned the pipeline of
    /// Device memory of the constant buffers TakeReusableDraw checks for texture handle writes.
    std::array<std::array<VideoCommon::Nx2FastDraw::DeviceSpans, FastDrawMaxwell::MaxConstBuffers>,
               FastDrawMaxwell::MaxShaderStage>
        handle_spans;
    FastDrawStats fast_draw_stats;
    /// Nx2FastDraw::Enabled and VerifyEnabled as the drawing thread last read them.
    bool fast_draw_on{};
    bool fast_draw_verify{};
    bool drawing_indirect{}; ///< DrawIndirect is preparing its draw
};

} // namespace Vulkan
