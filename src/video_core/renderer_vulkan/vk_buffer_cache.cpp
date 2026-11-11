// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/readback_diagnostics.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/host_shaders/vulkan_uint8_comp_spv.h"

#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/texture_cache/util.h"

namespace Vulkan {
namespace {
VkBufferCopy MakeBufferCopy(const VideoCommon::BufferCopy& copy) {
    return VkBufferCopy{
        .srcOffset = copy.src_offset,
        .dstOffset = copy.dst_offset,
        .size = copy.size,
    };
}

VkIndexType IndexTypeFromNumElements(const Device& device, u32 num_elements) {
    if (num_elements <= 0xff && device.IsExtIndexTypeUint8Supported()) {
        return VK_INDEX_TYPE_UINT8_EXT;
    }
    if (num_elements <= 0xffff) {
        return VK_INDEX_TYPE_UINT16;
    }
    return VK_INDEX_TYPE_UINT32;
}

size_t BytesPerIndex(VkIndexType index_type) {
    switch (index_type) {
    case VK_INDEX_TYPE_UINT8_EXT:
        return 1;
    case VK_INDEX_TYPE_UINT16:
        return 2;
    case VK_INDEX_TYPE_UINT32:
        return 4;
    default:
        ASSERT_MSG(false, "Invalid index type={}", index_type);
        return 1;
    }
}

vk::Buffer CreateBuffer(const Device& device, const MemoryAllocator& memory_allocator, u64 size,
                        VkDeviceSize sparse_alignment, MemoryUsage usage = MemoryUsage::DeviceLocal) {
    VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsExtConditionalRendering()) {
        flags |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (sparse_alignment > 1) {
        return memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal, sparse_alignment);
    }
    return memory_allocator.CreateBuffer(buffer_ci, usage);
}
} // Anonymous namespace

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams null_params)
    : VideoCommon::BufferBase(null_params), scheduler{&runtime.scheduler}, tracker{4096} {
    if (runtime.device.HasNullDescriptor()) {
        return;
    }
    device = &runtime.device;
    buffer = runtime.CreateNullBuffer();
    is_null = true;
    if (device->IsBufferDeviceAddressSupported()) {
        device_address = device->GetLogical().GetBufferDeviceAddress(*buffer);
    }
}

Buffer::Buffer(BufferCacheRuntime& runtime, DAddr cpu_addr_, u64 size_bytes_,
               bool sparse_compatible_)
    : VideoCommon::BufferBase(cpu_addr_, size_bytes_), device{&runtime.device},
      scheduler{&runtime.scheduler},
      tracker{SizeBytes()} {
    sparse_compatible = sparse_compatible_;
    readback_runtime = &runtime;
    const bool wanted = runtime.mapped_readback_policy.Wants(CpuAddr(), SizeBytes(), sparse_compatible);
    readback_lease = runtime.mapped_readback_policy.Reserve(CpuAddr(), SizeBytes(), sparse_compatible);
    if (readback_lease) {
        try {
            buffer = CreateBuffer(*device, runtime.memory_allocator, SizeBytes(), 1, MemoryUsage::Download);
            const auto properties = device->GetPhysical().GetMemoryProperties().memoryProperties;
            const auto flags = properties.memoryTypes[buffer.Location().memory_type].propertyFlags;
            if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
                readback_mapping = buffer.TryMapForReadback(SizeBytes());
        } catch (const vk::Exception& error) {
            if (error.GetResult() != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                error.GetResult() != VK_ERROR_OUT_OF_HOST_MEMORY) throw;
        }
    }
    if (readback_mapping.empty()) {
        buffer = {};
        readback_lease.reset();
        readback_promotion_failed = wanted;
        buffer = CreateBuffer(*device, runtime.memory_allocator, SizeBytes(),
                              runtime.SparseAlignmentFor(sparse_compatible));
    } else {
        // Register before initialization/copy commands so a mapped allocation never
        // infers a producer from writes that preceded its tracking lifetime.
        readback_writes = runtime.readback_writers.Register(Handle(), SizeBytes(), false);
        scheduler->EnableHostReadbackVisibility();
        std::fprintf(stderr, "MKW_MAPPED_BUFFER base=%llx size=%llu type=%u pool=%llu\n",
            static_cast<unsigned long long>(CpuAddr()), static_cast<unsigned long long>(SizeBytes()),
            buffer.Location().memory_type, static_cast<unsigned long long>(runtime.mapped_readback_policy.Used()));
    }

    if (runtime.device.HasDebuggingToolAttached()) {
        buffer.SetObjectNameEXT(fmt::format("Buffer {:#x}", CpuAddr()).c_str());
    }
    if (device->IsBufferDeviceAddressSupported()) {
        device_address = device->GetLogical().GetBufferDeviceAddress(*buffer);
    }
}

std::optional<VideoCommon::ReadbackRangeTracker::Ticket> Buffer::ReadbackProducer(u64 offset, u64 size) const {
    if (!readback_runtime || !readback_runtime->ReadbackTrackingEnabled()) return std::nullopt;
    if (!readback_writes) {
        // Admit buffers that are actually read repeatedly, not the first 256 GPU
        // allocations. Prior writes stay unknown until a new writer is recorded.
        if (readback_requests < 2) ++readback_requests;
        if (readback_requests < 2) return std::nullopt;
        readback_writes = readback_runtime->readback_writers.Register(Handle(), SizeBytes(), sparse_compatible);
    }
    return readback_writes ? readback_writes->Lookup(offset, size) : std::nullopt;
}

void Buffer::MarkUsage(u64 offset, u64 size) noexcept {
    tracker.Track(offset, size);
    last_usage_tick = scheduler->CurrentTick();
}

VkBufferView Buffer::View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format) {
    if (!device) {
        // Null buffer supported, return a null descriptor
        return VK_NULL_HANDLE;
    } else if (is_null) {
        // Null buffer not supported, adjust offset and size
        offset = 0;
        size = 0;
    }
    const auto it{std::ranges::find_if(views, [offset, size, format](const BufferView& view) {
        return offset == view.offset && size == view.size && format == view.format;
    })};
    if (it != views.end()) {
        return *it->handle;
    }
    views.push_back({
        .offset = offset,
        .size = size,
        .format = format,
        .handle = device->GetLogical().CreateBufferView({
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .buffer = *buffer,
            .format = MaxwellToVK::SurfaceFormat(*device, FormatType::Buffer, false, format).format,
            .offset = offset,
            .range = size,
        }),
    });
    return *views.back().handle;
}

class QuadIndexBuffer {
public:
    QuadIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                    Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
          staging_pool{staging_pool_} {}

    virtual ~QuadIndexBuffer() = default;

    void UpdateBuffer(u32 num_indices_) {
        if (num_indices_ <= num_indices) {
            return;
        }

        scheduler.Finish();

        num_indices = num_indices_;
        index_type = IndexTypeFromNumElements(device, num_indices);

        const u32 num_quads = GetQuadsNum(num_indices);
        const u32 num_triangle_indices = num_quads * 6;
        const u32 num_first_offset_copies = 4;
        const size_t bytes_per_index = BytesPerIndex(index_type);
        const size_t size_bytes = num_triangle_indices * bytes_per_index * num_first_offset_copies;
        const VkBufferCreateInfo buffer_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_bytes,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
        if (device.HasDebuggingToolAttached()) {
            buffer.SetObjectNameEXT("Quad LUT");
        }

        const bool host_visible = buffer.IsHostVisible();
        const StagingBufferRef staging = [&] {
            if (host_visible) {
                return StagingBufferRef{};
            }
            return staging_pool.Request(size_bytes, MemoryUsage::Upload);
        }();

        u8* staging_data = host_visible ? buffer.Mapped().data() : staging.mapped_span.data();
        const size_t quad_size = bytes_per_index * 6;

        for (u32 first = 0; first < num_first_offset_copies; ++first) {
            for (u32 quad = 0; quad < num_quads; ++quad) {
                MakeAndUpdateIndices(staging_data, quad_size, quad, first);
                staging_data += quad_size;
            }
        }

        if (!host_visible) {
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.Record([src_buffer = staging.buffer, src_offset = staging.offset,
                              dst_buffer = *buffer, size_bytes](vk::CommandBuffer cmdbuf) {
                const VkBufferCopy copy{
                    .srcOffset = src_offset,
                    .dstOffset = 0,
                    .size = size_bytes,
                };
                const VkBufferMemoryBarrier write_barrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_INDEX_READ_BIT,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = dst_buffer,
                    .offset = 0,
                    .size = size_bytes,
                };
                cmdbuf.CopyBuffer(src_buffer, dst_buffer, copy);
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, write_barrier);
            });
        } else {
            buffer.Flush();
        }
    }

    void BindBuffer(u32 first) {
        const VkIndexType index_type_ = index_type;
        const size_t sub_first_offset = static_cast<size_t>(first % 4) * GetQuadsNum(num_indices);
        const size_t offset =
            (sub_first_offset + GetQuadsNum(first)) * 6ULL * BytesPerIndex(index_type);
        scheduler.Record([buffer_ = *buffer, index_type_, offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(buffer_, offset, index_type_);
        });
    }

protected:
    virtual u32 GetQuadsNum(u32 num_indices) const = 0;

    virtual void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) = 0;

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;

    vk::Buffer buffer{};
    MemoryCommit memory_commit{};
    VkIndexType index_type{};
    u32 num_indices = 0;
};

class QuadArrayIndexBuffer : public QuadIndexBuffer {
public:
    QuadArrayIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_) {}

    ~QuadArrayIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ / 4;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 1, 2, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 4);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

class QuadStripIndexBuffer : public QuadIndexBuffer {
public:
    QuadStripIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_) {}

    ~QuadStripIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ >= 4 ? (num_indices_ - 2) / 2 : 0;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 3, 1, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 2);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_allocator_,
                                       Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                       GuestDescriptorQueue& guest_descriptor_queue_,
                                       ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                       DescriptorPool& descriptor_pool)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      staging_pool{staging_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      quad_index_pass(device, scheduler, descriptor_pool, staging_pool,
                      compute_pass_descriptor_queue),
      mapped_readback_policy([] {
          const char* value = std::getenv("EDEN_PS5_MAPPED_READBACK");
          return value && std::strcmp(value, "1") == 0;
      }()),
      readback_writers([] {
          const char* value = std::getenv("EDEN_PS5_READBACK_PRODUCERS");
          return value && std::strcmp(value, "1") == 0;
      }()),
      multi_range_buffers(device_) {
    const VkDriverIdKHR driver_id = device.GetDriverID();
    limit_dynamic_storage_buffers = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                    driver_id == VK_DRIVER_ID_ARM_PROPRIETARY;
    if (limit_dynamic_storage_buffers) {
        max_dynamic_storage_buffers = device.GetMaxDescriptorSetStorageBuffersDynamic();
    }    
    if (device.SupportsUint8Indices()) {
        uint8_pass = std::make_unique<Uint8Pass>(device, scheduler, descriptor_pool, staging_pool,
                                                 compute_pass_descriptor_queue);
    }
    quad_array_index_buffer = std::make_shared<QuadArrayIndexBuffer>(device_, memory_allocator_,
                                                                     scheduler_, staging_pool_);
    quad_strip_index_buffer = std::make_shared<QuadStripIndexBuffer>(device_, memory_allocator_,
                                                                     scheduler_, staging_pool_);
}

namespace {
class MappedVisibilityPass final : public ComputePass {
    static constexpr std::array<VkDescriptorSetLayoutBinding, 2> Bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
    static constexpr VkDescriptorUpdateTemplateEntry Update{
        0, 0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, sizeof(DescriptorUpdateEntry)};
    static constexpr DescriptorBankInfo Bank{.storage_buffers = 2, .score = 2};
public:
    MappedVisibilityPass(const Device& dev, Scheduler& sched, DescriptorPool& pool)
        : ComputePass(dev, sched, pool, Bindings, Update, Bank, {}, VULKAN_UINT8_COMP_SPV) {}
    void Run(Scheduler& sched, ComputePassDescriptorQueue& queue,
             VkBuffer input, VkBuffer output, u64 offset, u32 count) {
        queue.Acquire(sched, 2);
        queue.AddBuffer(input, 0, count);
        queue.AddBuffer(output, offset, count * sizeof(u16));
        const void* descriptors = queue.UpdateData();
        sched.RequestOutsideRenderPassOperationContext();
        sched.Record([this, descriptors, count](vk::CommandBuffer cmd) {
            const VkMemoryBarrier input_ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, input_ready);
            const VkDescriptorSet set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptors);
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmd.Dispatch((count + 1023) / 1024, 1, 1);
        });
    }
};
}

// Diagnostic visibility test; no guest RAM is imported or remapped.
void BufferCacheRuntime::ProbeMappedReadback(DescriptorPool& pool, ComputePassDescriptorQueue& queue) {
    const char* option = std::getenv("EDEN_PS5_MAPPED_READBACK_PROBE");
    if (!option || std::strcmp(option, "1") != 0) return;
    MappedVisibilityPass compute{device, scheduler, pool};
    u64 checked{}, failures{}, buffers{}, skipped{};
    for (const u64 bytes : {65536ULL, 2097152ULL}) {
        auto source = CreateBuffer(device, memory_allocator, bytes, 1);
        auto later = CreateBuffer(device, memory_allocator, bytes, 1);
        auto mapped = source.TryMapForReadback(bytes);
        const auto props = device.GetPhysical().GetMemoryProperties().memoryProperties;
        std::printf("MKW_MAPPED_ALLOCATION default_type=%u flags=%u mapped=%zu\n",
            source.Location().memory_type, props.memoryTypes[source.Location().memory_type].propertyFlags,
            mapped.size());
        if (mapped.empty()) {
            source = CreateBuffer(device, memory_allocator, bytes, 1, MemoryUsage::Download);
            mapped = source.TryMapForReadback(bytes);
            std::printf("MKW_MAPPED_ALLOCATION host_type=%u flags=%u mapped=%zu\n",
                source.Location().memory_type, props.memoryTypes[source.Location().memory_type].propertyFlags,
                mapped.size());
        }
        if (mapped.empty()) { ++skipped; continue; }
        ++buffers;
        std::vector<u32> expected(bytes / sizeof(u32), 0);
        ClearBuffer(*source, 0, bytes, 0);
        scheduler.Finish();
        for (u32 pass = 0; pass < 16; ++pass) {
            // Prime CPU cache lines with the previous completed contents.
            u64 warm{};
            for (u64 at = 0; at < bytes; at += 64)
                warm += *reinterpret_cast<const volatile u32*>(mapped.data() + at);
            const u64 offset = (pass % 4) * (bytes / 4);
            const u64 length = bytes / 4;
            const bool shader = (pass & 1) != 0;
            const u32 value = pass + 1;
            const u32 pattern = shader ? (value | (value << 16)) : (0xa5a50000U ^ (pass * 0x10203U));
            if (shader) {
                ClearBuffer(*later, 0, length / 2, value * 0x01010101U);
                compute.Run(scheduler, queue, *later, *source, offset, static_cast<u32>(length / 2));
            } else {
                ClearBuffer(*source, static_cast<u32>(offset), length, pattern);
            }
            scheduler.Record([](vk::CommandBuffer cmdbuf) {
                const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                      VK_PIPELINE_STAGE_HOST_BIT, 0, barrier);
            });
            const u64 producer = scheduler.Flush();
            // Keep later unrelated work in the next submission. Waiting for the
            // producer must not submit this newer command buffer.
            const u64 later_tick = scheduler.CurrentTick();
            ClearBuffer(*later, 0, 65536, pattern ^ 0xffffffffU);
            scheduler.Wait(producer);
            if (scheduler.CurrentTick() != later_tick) ++failures;
            std::fill_n(expected.begin() + offset / 4, length / 4, pattern);
            for (u64 word = 0; word < bytes / 4; ++word) {
                const u32 actual = reinterpret_cast<const volatile u32*>(mapped.data())[word];
                failures += actual != expected[word];
                ++checked;
            }
            // Independent staging comparison is validation overhead only.
            auto staging = DownloadStagingBuffer(bytes);
            const VideoCommon::BufferCopy copy{0, staging.offset, bytes};
            CopyBuffer(staging.buffer, *source, std::span{&copy, 1}, true);
            scheduler.Finish();
            for (u64 word = 0; word < bytes / 4; ++word) {
                u32 actual{};
                std::memcpy(&actual, staging.mapped_span.data() + word * 4, 4);
                failures += actual != expected[word];
                ++checked;
            }
            std::printf("MKW_MAPPED_PROBE size=%llu pass=%u shader=%u producer=%llu later=%llu warm=%llu failures=%llu\n",
                static_cast<unsigned long long>(bytes), pass, unsigned(shader),
                static_cast<unsigned long long>(producer), static_cast<unsigned long long>(later_tick),
                static_cast<unsigned long long>(warm), static_cast<unsigned long long>(failures));
        }
    }
    std::printf("MKW_MAPPED_PROBE_RESULT buffers=%llu skipped=%llu checked=%llu failures=%llu\n",
        static_cast<unsigned long long>(buffers), static_cast<unsigned long long>(skipped),
        static_cast<unsigned long long>(checked), static_cast<unsigned long long>(failures));
}

StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) {
    VideoCommon::ReadbackDiagnostics::Scope diagnostic{
        VideoCommon::ReadbackDiagnostics::Event::UploadStaging, size};
    return staging_pool.Request(size, MemoryUsage::Upload);
}

StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    VideoCommon::ReadbackDiagnostics::Scope diagnostic{
        deferred ? VideoCommon::ReadbackDiagnostics::Event::DeferredDownloadStaging
                 : VideoCommon::ReadbackDiagnostics::Event::DownloadStaging,
        size};
    return staging_pool.Request(size, MemoryUsage::Download, deferred);
}

VkFormat BufferCacheRuntime::TexelBufferFormat(VideoCore::Surface::PixelFormat format) const {
    return MaxwellToVK::SurfaceFormat(device, FormatType::Buffer, false, format).format;
}

void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_pool.FreeDeferred(ref);
}

u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool BufferCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

u32 BufferCacheRuntime::GetUniformBufferAlignment() const {
    return static_cast<u32>(device.GetUniformBufferAlignment());
}

u32 BufferCacheRuntime::GetStorageBufferAlignment() const {
    return static_cast<u32>(device.GetStorageBufferAlignment());
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept {
    readback_writers.Prune();
    for (auto it = slot_buffers.begin(); it != slot_buffers.end(); it++) {
        if (scheduler.IsFree(it->LastUsageTick())) {
            it->ResetUsageTracking();
        }
    }
}

bool BufferCacheRuntime::BindSmallUniformReusing(
    Tegra::MaxwellDeviceMemoryManager& device_memory, size_t stage, u32 index,
    DAddr device_addr, u32 size) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_UBOREUSE");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    if (!enabled || size == 0 || size > UniformStreamReuse::MaxBytes || stage >= 5 || index >= 18) {
        return false;
    }
    if (!uniform_stream_reuse) {
        uniform_stream_reuse = std::make_unique<UniformStreamReuse>();
    }
    auto& reuse = *uniform_stream_reuse;
    auto& slot = reuse.slots[stage][index];
    device_memory.ReadBlockUnsafe(device_addr, reuse.scratch.data(), size);
    if (slot.size != size || slot.tick != scheduler.CurrentTick() ||
        std::memcmp(slot.bytes.data(), reuse.scratch.data(), size) != 0) {
        slot.ref = staging_pool.Request(size, MemoryUsage::Upload, true);
        staging_pool.FreeDeferred(slot.ref);
        std::memcpy(slot.ref.mapped_span.data(), reuse.scratch.data(), size);
        std::memcpy(slot.bytes.data(), reuse.scratch.data(), size);
        slot.size = size;
        slot.tick = scheduler.CurrentTick();
    }
    guest_descriptor_queue.AddBuffer(slot.ref.buffer, slot.ref.device_address,
                                    static_cast<u32>(slot.ref.offset), size);
    return true;
}

extern "C" unsigned eden_mapped_readback_mode() __attribute__((weak));
extern "C" void eden_mapped_readback_result(u64, u64, u64) __attribute__((weak));
extern "C" void eden_mapped_readback_reject(unsigned) __attribute__((weak));

unsigned BufferCacheRuntime::MappedReadbackMode() const {
    return mapped_readback_failed || !eden_mapped_readback_mode ? 0 : eden_mapped_readback_mode();
}

bool BufferCacheRuntime::NeedsReadbackPromotion(const Buffer& buffer) {
    if (buffer.IsSparseCompatible() || buffer.readback_promotion_failed ||
        !buffer.readback_mapping.empty() || buffer.readback_requests < 2 ||
        !ReadbackTrackingEnabled()) return false;
    return mapped_readback_policy.Prefer(buffer.CpuAddr(), buffer.SizeBytes());
}

std::span<const u8> BufferCacheRuntime::TryMappedReadback(
    const Buffer& buffer, std::span<const VideoCommon::BufferCopy> copies) {
    const auto reject = [](unsigned reason) -> std::span<const u8> {
        if (eden_mapped_readback_reject) eden_mapped_readback_reject(reason);
        return {};
    };
    if (!MappedReadbackMode()) return {};
    if (buffer.IsSparseCompatible() || buffer.readback_mapping.empty() ||
        !buffer.readback_writes || copies.empty()) return reject(0);
    if (buffer.readback_writes->external_writer) return reject(1);
    for (const auto& copy : copies) {
        const auto ticket = buffer.readback_writes->Lookup(copy.src_offset, copy.size);
        if (!ticket) return reject(2);
        if (!scheduler.IsFree(ticket->producer)) return reject(3);
    }
    // No wait or submission here: all known producers are already complete. The
    // GPU/cache owner prevents recording a newer writer during immediate copying.
    return buffer.readback_mapping;
}

void BufferCacheRuntime::ValidateMappedReadback(std::span<const u8> expected,
                                               std::span<const u8> actual) {
    const bool mismatch = expected.size() != actual.size() ||
                          !std::equal(expected.begin(), expected.end(), actual.begin());
    mapped_readback_failed |= mismatch;
    if (eden_mapped_readback_result) eden_mapped_readback_result(0, expected.size(), mismatch);
}

void BufferCacheRuntime::RecordDirectReadback(u64 bytes) {
    if (eden_mapped_readback_result) eden_mapped_readback_result(bytes, 0, 0);
}

void BufferCacheRuntime::CompleteReadbackWriteBatch() {
    readback_writers.CompleteBatch(scheduler.CurrentTick());
}

u64 BufferCacheRuntime::CurrentTick() {
    return scheduler.CurrentTick();
}

bool BufferCacheRuntime::IsFree(u64 tick) {
    return scheduler.IsFree(tick);
}

void BufferCacheRuntime::Wait(u64 tick) {
    scheduler.Wait(tick);
}

void BufferCacheRuntime::Finish() {
    VideoCommon::ReadbackDiagnostics::Scope diagnostic{
        VideoCommon::ReadbackDiagnostics::Event::BufferFinish};
    scheduler.Finish();
}

bool BufferCacheRuntime::CanReorderUpload(const Buffer& buffer,
                                          std::span<const VideoCommon::BufferCopy> copies) {
    if (Settings::values.disable_buffer_reorder) {
        return false;
    }
    const bool can_use_upload_cmdbuf =
        std::ranges::all_of(copies, [&](const VideoCommon::BufferCopy& copy) {
            return !buffer.IsRegionUsed(copy.dst_offset, copy.size);
        });
    return can_use_upload_cmdbuf;
}

void BufferCacheRuntime::CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                                    bool can_reorder_upload) {
    if (dst_buffer == VK_NULL_HANDLE || src_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    // Measuring a popular game, this number never exceeds the specified size once data is warmed up
    boost::container::small_vector<VkBufferCopy, 8> vk_copies(copies.size());
    std::ranges::transform(copies, vk_copies.begin(), MakeBufferCopy);
    if (src_buffer == staging_pool.StreamBuf() && can_reorder_upload) {
        scheduler.RecordWithUploadBuffer([src_buffer, dst_buffer, vk_copies](
                                             vk::CommandBuffer, vk::CommandBuffer upload_cmdbuf) {
            upload_cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        });
        for (const auto& copy : copies)
            readback_writers.TransferWrite(dst_buffer, copy.dst_offset, copy.size, scheduler.CurrentTick(), true);
        return;
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([src_buffer, dst_buffer, vk_copies, barrier](vk::CommandBuffer cmdbuf) {
        if (barrier) {
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, READ_BARRIER);
        }
        cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        if (barrier) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   vk::PIPELINE_STAGE_GRAPHICS_COMPUTE, 0, WRITE_BARRIER);
        }
    });
    for (const auto& copy : copies)
        readback_writers.TransferWrite(dst_buffer, copy.dst_offset, copy.size, scheduler.CurrentTick(), false);

}

void BufferCacheRuntime::PreCopyBarrier() {
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
    });
}

void BufferCacheRuntime::PostCopyBarrier() {
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                               0, WRITE_BARRIER);
    });
}

void BufferCacheRuntime::ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value) {
    if (dest_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dest_buffer, offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
        cmdbuf.FillBuffer(dest_buffer, offset, size, value);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                               0, WRITE_BARRIER);
    });
    readback_writers.TransferWrite(dest_buffer, offset, size, scheduler.CurrentTick(), false);

}

bool BufferCacheRuntime::BindMultiRangeStorageBuffer(u64 key, bool is_written) {
    if (multi_range_sources.empty() || multi_range_total == 0) {
        return false;
    }
    const MultiRangeRef ref = multi_range_buffers.Get(device, scheduler, memory_allocator, key,
                                                     multi_range_sources, multi_range_total);
    if (ref.handle == VK_NULL_HANDLE) {
        return false;
    }
    if (is_written && !ref.sparse) {
        return false;
    }
    if (ref.needs_gather) {
        PreCopyBarrier();
        VkDeviceSize dst_offset = 0;
        for (const MultiRangeSource& source : multi_range_sources) {
            const std::array<VideoCommon::BufferCopy, 1> copy{VideoCommon::BufferCopy{
                .src_offset = u64(source.offset),
                .dst_offset = u64(dst_offset),
                .size = size_t(source.size),
            }};
            CopyBuffer(ref.handle, source.handle, copy, false);
            dst_offset += source.size;
        }
        PostCopyBarrier();
        multi_range_buffers.MarkGathered(key);
    }
    guest_descriptor_queue.AddBuffer(ref.handle, ref.address, 0, ref.size);
    return true;
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format,
                                         u32 base_vertex, u32 num_indices, VkBuffer buffer,
                                         u32 offset, [[maybe_unused]] u32 size) {
    VkIndexType vk_index_type = MaxwellToVK::IndexFormat(index_format);
    VkDeviceSize vk_offset = offset;
    VkBuffer vk_buffer = buffer;
    if (topology == PrimitiveTopology::Quads || topology == PrimitiveTopology::QuadStrip) {
        vk_index_type = VK_INDEX_TYPE_UINT32;
        std::tie(vk_buffer, vk_offset) =
            quad_index_pass.Assemble(index_format, num_indices, base_vertex, buffer, offset,
                                     topology == PrimitiveTopology::QuadStrip);
    } else if (vk_index_type == VK_INDEX_TYPE_UINT8_EXT && !device.IsExtIndexTypeUint8Supported()) {
        vk_index_type = VK_INDEX_TYPE_UINT16;
        if (uint8_pass) {
            std::tie(vk_buffer, vk_offset) = uint8_pass->Assemble(num_indices, buffer, offset);
        } else if (device.GetDriverID() == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
            ReserveNullBuffer();
            vk_buffer = *null_buffer;
            vk_offset = 0;
        }
    }
    if (vk_buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null index buffers. Replace it with our own null buffer.
        ReserveNullBuffer();
        vk_buffer = *null_buffer;
    }
    scheduler.Record([vk_buffer, vk_offset, vk_index_type](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindIndexBuffer(vk_buffer, vk_offset, vk_index_type);
    });
}

void BufferCacheRuntime::BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count) {
    if (count == 0) {
        ReserveNullBuffer();
        scheduler.Record([this](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(*null_buffer, 0, VK_INDEX_TYPE_UINT32);
        });
        return;
    }

    if (topology == PrimitiveTopology::Quads) {
        quad_array_index_buffer->UpdateBuffer(first + count);
        quad_array_index_buffer->BindBuffer(first);
    } else if (topology == PrimitiveTopology::QuadStrip) {
        quad_strip_index_buffer->UpdateBuffer(first + count);
        quad_strip_index_buffer->BindBuffer(first);
    }
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size, u32 stride) {
    if (index >= device.GetMaxVertexInputBindings()) {
        return;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        scheduler.Record([index, buffer, offset, size, stride](vk::CommandBuffer cmdbuf) {
            const VkDeviceSize vk_offset = buffer != VK_NULL_HANDLE ? offset : 0;
            const VkDeviceSize vk_size = buffer != VK_NULL_HANDLE ? size : VK_WHOLE_SIZE;
            const VkDeviceSize vk_stride = stride;
            cmdbuf.BindVertexBuffers2EXT(index, 1, &buffer, &vk_offset, &vk_size, &vk_stride);
        });
    } else {
        if (!device.HasNullDescriptor() && buffer == VK_NULL_HANDLE) {
            ReserveNullBuffer();
            buffer = *null_buffer;
            offset = 0;
        }
        scheduler.Record([index, buffer, offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffer(index, buffer, offset);
        });
    }
}

void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    boost::container::static_vector<VkBuffer, VideoCommon::NUM_VERTEX_BUFFERS> buffer_handles(bindings.buffers.size());
    for (u32 i = 0; i < bindings.buffers.size(); ++i) {
        auto handle = bindings.buffers[i]->Handle();
        if (handle == VK_NULL_HANDLE) {
            bindings.offsets[i] = 0;
            bindings.sizes[i] = VK_WHOLE_SIZE;
            if (!device.HasNullDescriptor()) {
                ReserveNullBuffer();
                handle = *null_buffer;
            }
        }
        buffer_handles[i] = handle;
    }
    const u32 device_max = device.GetMaxVertexInputBindings();
    const u32 min_binding = (std::min)(bindings.min_index, device_max);
    const u32 max_binding = (std::min)(bindings.max_index, device_max);
    const u32 binding_count = max_binding - min_binding;
    if (binding_count == 0) {
        return;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        scheduler.Record([bindings_ = std::move(bindings), buffer_handles_ = std::move(buffer_handles), binding_count](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffers2EXT(bindings_.min_index, binding_count, buffer_handles_.data(), bindings_.offsets.data(), bindings_.sizes.data(), bindings_.strides.data());
        });
    } else {
        scheduler.Record([bindings_ = std::move(bindings), buffer_handles_ = std::move(buffer_handles), binding_count](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffers(bindings_.min_index, binding_count, buffer_handles_.data(), bindings_.offsets.data());
        });
    }
}

void BufferCacheRuntime::BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset,
                                                     u32 size) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    if (buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null transform feedback buffers.
        // Replace it with our own null buffer.
        ReserveNullBuffer();
        buffer = *null_buffer;
        offset = 0;
        size = 0;
    }
    scheduler.Record([index, buffer, offset, size](vk::CommandBuffer cmdbuf) {
        const VkDeviceSize vk_offset = offset;
        const VkDeviceSize vk_size = size;
        cmdbuf.BindTransformFeedbackBuffersEXT(index, 1, &buffer, &vk_offset, &vk_size);
    });
}

void BufferCacheRuntime::BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    boost::container::static_vector<VkBuffer, VideoCommon::NUM_VERTEX_BUFFERS> buffer_handles(bindings.buffers.size());
    for (u32 i = 0; i < bindings.buffers.size(); ++i) {
        auto handle = bindings.buffers[i]->Handle();
        if (handle == VK_NULL_HANDLE) {
            ReserveNullBuffer();
            handle = *null_buffer;
            bindings.offsets[i] = 0;
            bindings.sizes[i] = 0;
        }
        buffer_handles[i] = handle;
    }
    scheduler.Record([bindings_ = std::move(bindings), buffer_handles_ = std::move(buffer_handles)](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindTransformFeedbackBuffersEXT(0, u32(buffer_handles_.size()), buffer_handles_.data(), bindings_.offsets.data(), bindings_.sizes.data());
    });
}

void BufferCacheRuntime::ReserveNullBuffer() {
    if (!null_buffer) {
        null_buffer = CreateNullBuffer();
    }
}

vk::Buffer BufferCacheRuntime::CreateNullBuffer() {
    VkBufferCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 4,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer ret = memory_allocator.CreateBuffer(create_info, MemoryUsage::DeviceLocal);
    if (device.HasDebuggingToolAttached()) {
        ret.SetObjectNameEXT("Null buffer");
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([buffer = *ret](vk::CommandBuffer cmdbuf) {
        cmdbuf.FillBuffer(buffer, 0, VK_WHOLE_SIZE, 0);
    });

    return ret;
}

} // namespace Vulkan
