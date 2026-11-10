// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Development hang watchdog (PS5). Two sessions on Ocarina of Time froze with a black screen and
// left nothing behind: both logs stop in the middle of the periodic report, with no
// VK_ERROR_DEVICE_LOST, no "the GPU has not reached submission" line and no crash file, so the
// process either froze every thread or died before the GPU watchdog could speak.
//
// This notices that presents stopped and writes what is still readable from inside the process:
// how long the silence lasted, the frame it stopped at, and every guest thread with its state,
// wait reason, active core and saved-context backtrace (CrashSite::DumpThreads). The lines go
// straight to stderr with an fflush behind them, so nothing sits in a buffer when the process
// dies; the guest dump is written after them, so a logging lock that is itself stuck still leaves
// the first line in the file.
//
// EDEN_HANG_WATCHDOG_SECS (env.txt) names the silence that counts as a hang: default 10, 0 turns
// the watchdog off. While the silence lasts the dump repeats every interval, at most six times.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <utility>

namespace Eden::HangWatch {

inline std::atomic<double> last_present_seconds{0.0};
inline std::atomic<unsigned long long> frames_at_last_present{0};
inline std::atomic<bool> started{false};

inline double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline unsigned SilenceSeconds() {
    const char* value = std::getenv("EDEN_HANG_WATCHDOG_SECS");
    if (value == nullptr || value[0] == '\0') return 10;
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 ? static_cast<unsigned>(parsed) : 0;
}

// The renderer calls this for every frame it hands to the present manager.
inline void NotePresent(double now, unsigned long long frames) {
    frames_at_last_present.store(frames, std::memory_order_relaxed);
    last_present_seconds.store(now, std::memory_order_relaxed);
}

// dump() runs after the first line is flushed; on the console it is CrashSite::DumpThreads.
inline void Start(std::function<void(const char*)> dump) {
    const unsigned silence = SilenceSeconds();
    if (silence == 0) return;
    if (started.exchange(true, std::memory_order_relaxed)) return;
    std::thread([silence, dump = std::move(dump)] {
        unsigned reported = 0;
        double last_report = 0.0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const double last = last_present_seconds.load(std::memory_order_relaxed);
            if (last <= 0.0) continue;
            const double now = Now();
            const double quiet = now - last;
            if (quiet < static_cast<double>(silence)) {
                // Presents came back: the next silence is a new hang and gets its own six.
                reported = 0;
                last_report = 0.0;
                continue;
            }
            if (reported >= 6) continue;
            // One dump per interval of silence, not one per second.
            if (last_report > 0.0 && now - last_report < static_cast<double>(silence)) continue;
            last_report = now;
            ++reported;
            std::fprintf(stderr, "EDEN_HANG no_present_s=%.1f frames=%llu report=%u of 6\n", quiet,
                         frames_at_last_present.load(std::memory_order_relaxed), reported);
            std::fflush(stderr);
            dump("hang");
        }
    }).detach();
}

} // namespace Eden::HangWatch
