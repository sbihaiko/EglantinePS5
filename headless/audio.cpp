// SPDX-License-Identifier: GPL-3.0-or-later
#include "devices.h"
#include "preferences.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include "audio_core/sink/null_sink.h"
#include "common/logging.h"
#include "common/stall_ledger.h"
#include "native_audio.hpp"
#include "audio_out_init.h"

namespace Eden {
using namespace AudioCore::Sink;
using namespace ps5::native_audio;

namespace {
// EDEN_NX2_STALL_REPORT (common/stall_ledger.h): the cuts a player hears, on the output thread.
// A block is 256 frames at 48 kHz (5.33 ms), and sceAudioOutOutput returns once the port takes
// the next one: a submit more than a block after the last one returned found the port dry (late;
// its gap is the excess, an upper bound if the port holds more than a block). A block the queue
// could not fill repeats the last frame (SinkStream::ProcessAudioOutAndRender): the blocks of
// one such run are one dry gap. A gap over 50 ms is left for the frame report to print
// (EDEN_AUDIO_GAP, at most one a second); the longest of each frame goes on its EDEN_STALL line.
class AudioGaps {
public:
    using Clock = Common::StallLedger::Clock;
    using Category = Common::StallLedger::Category;

    void Paused() {
        returned = {};
        dry_frames = 0;
    }
    // After a block was rendered: `held` of its frames repeat the last one.
    void Rendered(u64 held) {
        if (!Common::StallLedger::Enabled()) return;
        if (held < kAudioOutGrain) EndDry(); // the block began with queued frames
        if (held == 0) return;
        dry_frames += held;
        Common::StallLedger::NoteMax(Category::AudioDry, FramesNs(dry_frames));
    }
    void Submitting() {
        if (!Common::StallLedger::Enabled() || returned == Clock::time_point{}) return;
        const auto now = Clock::now();
        const auto since = now - returned;
        if (since <= kBlock) return;
        const auto gap = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(since - kBlock).count());
        Common::StallLedger::Add(Category::AudioLate, gap, 1, 0);
        Common::StallLedger::NoteMax(Category::AudioLate, gap);
        Post(now, gap, false);
    }
    void Submitted() {
        if (Common::StallLedger::Enabled()) returned = Clock::now();
    }

private:
    static constexpr auto kBlock = std::chrono::nanoseconds{1'000'000'000ULL * kAudioOutGrain / kAudioOutRate};

    static u64 FramesNs(u64 frames) {
        return frames * 1'000'000'000ULL / kAudioOutRate;
    }
    void EndDry() {
        if (dry_frames == 0) return;
        Post(Clock::now(), FramesNs(dry_frames), true);
        dry_frames = 0;
    }
    static void Post(Clock::time_point now, u64 gap_ns, bool dry) {
        static std::atomic<long long> last_ms{-1000};
        const u64 gap_ms = gap_ns / 1'000'000;
        if (gap_ms <= 50) return;
        const long long now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - Common::StallLedger::process_start).count();
        long long last = last_ms.load(std::memory_order_relaxed);
        if (now_ms - last < 1000 || !last_ms.compare_exchange_strong(last, now_ms, std::memory_order_relaxed)) return;
        Common::StallLedger::PostAudioGap(now, gap_ms, dry);
    }

    Clock::time_point returned{};
    u64 dry_frames = 0;
};
} // namespace

AudioStream::AudioStream(Core::System& system, u32 channels, const std::string& name_, StreamType type)
    : SinkStream{system, type} {
    if (type == StreamType::In || (channels != 1 && channels != 2 && channels != 6))
        throw std::invalid_argument("Unsupported AudioOut stream format");
    name = name_;
    system_channels = channels;
    device_channels = 2;
    SetRingSize(4);
    if (!AudioOutReady()) throw std::runtime_error("sceAudioOutInit failed");
    handle = sceAudioOutOpen(0xff, 0, 0, kAudioOutGrain, kAudioOutRate, kAudioOutStereoS16);
    if (handle < 0) throw std::runtime_error("sceAudioOutOpen failed");
    std::array<int, 8> volumes;
    volumes.fill(AudioVolume(PreferencesFor(session_title.load())));
    if (sceAudioOutSetVolume(handle, 3, volumes.data()) < 0) {
        sceAudioOutClose(handle);
        handle = -1;
        throw std::runtime_error("sceAudioOutSetVolume failed");
    }
    try {
        worker = std::jthread([this](std::stop_token stop) {
            std::array<s16, kAudioOutGrain * 2> block{};
            AudioGaps gaps;
            std::unique_lock lock(mutex);
            while (!stop.stop_requested()) {
                if (Common::StallLedger::Enabled() && IsPaused()) gaps.Paused();
                if (!wake.wait(lock, stop, [this] { return !IsPaused(); })) break;
                const u64 held_before = held_frames;
                ProcessAudioOutAndRender(block, kAudioOutGrain);
                gaps.Rendered(held_frames - held_before);
                // AudioOut paces complete 256-frame, 48 kHz stereo blocks.
                in_flight = true;
                lock.unlock();
                gaps.Submitting();
                const int result = sceAudioOutOutput(handle, block.data());
                gaps.Submitted();
                lock.lock();
                in_flight = false;
                wake.notify_all();
                if (result < 0) {
                    failed = true;
                    SignalPause();
                    LOG_ERROR(Audio_Sink, "PS5 AudioOut output failed");
                    break;
                }
                output_frames += kAudioOutGrain;
                for (std::size_t frame = 0; frame < kAudioOutGrain; ++frame)
                    nonzero_frames += block[frame * 2] != 0 || block[frame * 2 + 1] != 0;
            }
        });
    } catch (...) {
        sceAudioOutClose(handle);
        handle = -1;
        throw;
    }
}
AudioStream::~AudioStream() { Finalize(); }
void AudioStream::Start(bool) {
    std::scoped_lock lock(mutex);
    if (handle >= 0 && !failed) { paused = false; wake.notify_one(); }
}
void AudioStream::Stop() {
    SignalPause();
    std::unique_lock lock(mutex);
    wake.wait(lock, [this] { return !in_flight; });
}
void AudioStream::Finalize() {
    worker.request_stop();
    wake.notify_all();
    if (worker.joinable()) worker.join();
    std::scoped_lock lock(mutex);
    SignalPause();
    if (handle >= 0) {
        const int drain = sceAudioOutOutput(handle, nullptr);
        const int close = sceAudioOutClose(handle);
        LOG_INFO(Audio_Sink, "EDEN_AUDIO_PORT_CLOSED frames={} nonzero={} failed={} drain={} close={}",
                 output_frames, nonzero_frames, failed.load(), drain, close);
        handle = -1;
    }
}
void AudioStream::AppendBuffer(SinkBuffer& buffer, std::span<s16> samples) {
    std::scoped_lock lock(mutex); // Publish PCM and its descriptor before the consumer runs.
    if (buffer.frames == 0 || buffer.frames > 0x8000 || buffer.frames_played != 0 ||
        samples.size() != buffer.frames * system_channels)
        throw std::invalid_argument("Invalid PCM buffer dimensions");
    if (failed || handle < 0) throw std::runtime_error("AudioOut stream is unavailable");
    SinkStream::AppendBuffer(buffer, samples);
}
SinkStream* AudioSink::AcquireSinkStream(Core::System& system, u32 channels,
                                        const std::string& name, StreamType type) {
    system_channels = channels;
    // Microphone capture is outside this output adapter.
    SinkStreamPtr stream = type == StreamType::In ?
        SinkStreamPtr{std::make_unique<NullSinkStreamImpl>(system, type)} :
        SinkStreamPtr{std::make_unique<AudioStream>(system, channels, name, type)};
    stream->SetDeviceVolume(device_volume);
    stream->SetSystemVolume(system_volume);
    auto* result = stream.get();
    streams.push_back(std::move(stream));
    return result;
}
void AudioSink::CloseStream(SinkStream* stream) {
    std::erase_if(streams, [stream](const auto& item) { return item.get() == stream; });
}
void AudioSink::SetDeviceVolume(f32 value) {
    device_volume = std::clamp(value, 0.0f, 1.0f);
    for (auto& stream : streams) stream->SetDeviceVolume(device_volume);
}
void AudioSink::SetSystemVolume(f32 value) {
    system_volume = std::clamp(value, 0.0f, 1.0f);
    for (auto& stream : streams) stream->SetSystemVolume(system_volume);
}
}
