// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>

#include "core/hle/kernel/k_spin_lock.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace Kernel {

namespace {

// Spins of about 30-140 cycles each before a contended Lock sleeps (a few microseconds).
constexpr int SpinCount = 128;

void Pause() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

} // Anonymous namespace

void KSpinLock::Lock() {
    u32 expected = 0;
    if (m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
        return;
    }
    // The scheduler lock is contended by six guest cores handing work to each other ~14,000
    // times a second at Mario Kart World's title; the atomic wait below polls the clock before
    // it sleeps (2.7% of the guest cores' time on the console). EDEN_PS5_KSPIN=<pauses> (2048)
    // spins longer first; 128 restores the old count.
    static const int spin_count = [] {
        const char* value = std::getenv("EDEN_PS5_KSPIN");
        return value != nullptr ? std::atoi(value) : 2048;
    }();
    for (int spin = 0; spin < spin_count; ++spin) {
        Pause();
        if (m_state.load(std::memory_order_relaxed) == 0) {
            expected = 0;
            if (m_state.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                return;
            }
        }
    }
    // Mark the lock contended and sleep until it is released; taken this way it stays marked,
    // so its Unlock wakes the next sleeper.
    while (m_state.exchange(2, std::memory_order_acquire) != 0) {
        m_state.wait(2, std::memory_order_relaxed);
    }
}

void KSpinLock::Unlock() {
    if (m_state.exchange(0, std::memory_order_release) == 2) {
        m_state.notify_one();
    }
}

bool KSpinLock::TryLock() {
    u32 expected = 0;
    return m_state.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                           std::memory_order_relaxed);
}

} // namespace Kernel
