// SPDX-FileCopyrightText: Copyright 2026 Eglantine Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <utility>

#include "common/polyfill_thread.h"

namespace Common {

// Publishing host work must not wait for the consumer to execute earlier work.
// Producers can hold a memory-registry lock that the GPU consumer needs before
// it can finish its current command. A bounded queue creates a lock cycle there.
template <typename T>
class HostCommandQueue {
public:
    template <typename... Args>
    void Emplace(Args&&... args) {
        {
            std::scoped_lock lock{mutex};
            entries.emplace_back(std::forward<Args>(args)...);
        }
        available.notify_one();
    }

    bool TryPop(T& out) {
        std::scoped_lock lock{mutex};
        if (entries.empty()) {
            return false;
        }
        out = std::move(entries.front());
        entries.pop_front();
        return true;
    }

    void PopWait(T& out, std::stop_token stop) {
        // The guest's next command usually follows within microseconds of the last one:
        // EDEN_PS5_GPU_SPIN_US=<n> polls for that long before sleeping (0: sleep at once).
        if (const long long spin = SpinNanoseconds(); spin > 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds{spin};
            while (!stop.stop_requested()) {
                if (TryPop(out)) return;
                if (std::chrono::steady_clock::now() >= deadline) break;
#if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
#endif
            }
        }
        std::unique_lock lock{mutex};
        if (!available.wait(lock, stop, [this] { return !entries.empty(); })) {
            return;
        }
        out = std::move(entries.front());
        entries.pop_front();
    }

private:
    static long long SpinNanoseconds() {
        static const long long value = [] {
            const char* text = std::getenv("EDEN_PS5_GPU_SPIN_US");
            return text != nullptr ? std::strtoll(text, nullptr, 10) * 1000 : 0LL;
        }();
        return value;
    }
    std::mutex mutex;
    std::condition_variable_any available;
    std::deque<T> entries;
};

} // namespace Common
