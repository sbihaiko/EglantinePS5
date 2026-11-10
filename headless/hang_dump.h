// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The guest-thread dump the hang watchdog asks for, written straight to stderr.
//
// CrashSite::DumpThreads prints the same facts through LOG_CRITICAL, and on the console that was
// the wrong way round: the Eden log reaches its 100 MiB cap and stops taking lines, so the dump of
// the 02:18 hang went into a log whose last entry was at t=95.6 s and never reached the disk. The
// watchdog can still write to stderr when the picture is black (it does, every ten seconds), so
// the dump goes there too: no logging thread, no queue, no cap.
//
// One line per thread. `frames` is the saved-context chain, so a thread that is waiting shows
// where it last switched out — module+offset, which tools/symbolize-sites.py resolves.
#include <cstdio>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/arm/debug.h"

namespace Eden::HangDump {

template <typename Process>
void Threads(const char* reason, Process& process) {
    unsigned long long count = 0;
    for (const auto& thread : process.GetThreadList()) {
        ++count;
        const auto name = Core::GetThreadName(&thread);
        std::fprintf(stderr, "EDEN_HANG_THREAD %s id=%llu name=%s state=%s wait=%s core=%u prio=%u frames=",
                     reason, static_cast<unsigned long long>(thread.GetThreadId()),
                     name ? name->c_str() : "-", std::string(Core::GetThreadState(&thread)).c_str(),
                     std::string(Core::GetThreadWaitReason(&thread)).c_str(),
                     static_cast<unsigned>(thread.GetActiveCore()),
                     static_cast<unsigned>(thread.GetPriority()));
        for (const auto& entry : Core::GetBacktrace(&thread))
            std::fprintf(stderr, " %s+%llX", entry.module.c_str(),
                         static_cast<unsigned long long>(entry.offset));
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "EDEN_HANG_THREADS reason=%s count=%llu\n", reason, count);
    std::fflush(stderr);
}

} // namespace Eden::HangDump
