// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stop_token>

namespace Common {

// One synchronous command at a time per submitting host thread. A queued command keeps
// its own reference through Signal(), even if Wait() returns and the caller exits.
class HostCommandCompletion final {
public:
    static std::shared_ptr<HostCommandCompletion> ForCurrentThread() {
        thread_local auto completion = std::make_shared<HostCommandCompletion>();
        if (!completion->Reusable()) completion = std::make_shared<HostCommandCompletion>();
        completion->Reset();
        return completion;
    }

    void Wait() {
        // A command the GPU thread answers in microseconds is not worth a sleep and a wake-up:
        // EDEN_PS5_READ_SPIN_US=<n> polls for that long first (0: block at once).
        if (const long long spin = SpinNanoseconds(); spin > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds{spin};
            while (!done_flag.load(std::memory_order_acquire)) {
                if (std::chrono::steady_clock::now() >= deadline) break;
                PauseCpu();
            }
            if (done_flag.load(std::memory_order_acquire)) return;
        }
        std::unique_lock lock{mutex};
        ready.wait(lock, [this] { return done; });
    }
    bool Wait(std::stop_token stop) {
        std::unique_lock lock{mutex};
        if (ready.wait(lock, stop, [this] { return done; })) return true;
        reusable = false; // A late callback must never complete a later request.
        return false;
    }
    void Signal() {
        std::lock_guard lock{mutex};
        done = true;
        done_flag.store(true, std::memory_order_release);
        ready.notify_one();
    }
    static long long SpinNanoseconds() {
        static const long long value = [] {
            const char* text = std::getenv("EDEN_PS5_READ_SPIN_US");
            return text != nullptr ? std::strtoll(text, nullptr, 10) * 1000 : 0LL;
        }();
        return value;
    }
    static void PauseCpu() {
#if defined(__x86_64__) || defined(_M_X64)
        __builtin_ia32_pause();
#endif
    }

private:
    bool Reusable() {
        std::lock_guard lock{mutex};
        return reusable;
    }
    void Reset() {
        std::lock_guard lock{mutex};
        done = false;
        done_flag.store(false, std::memory_order_release);
    }
    std::mutex mutex;
    std::atomic<bool> done_flag{};
    std::condition_variable_any ready;
    bool done{};
    bool reusable{true};
};

} // namespace Common
