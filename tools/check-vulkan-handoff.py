#!/usr/bin/env python3
"""Run generated scheduler handoff methods against a blocking command fixture."""
from pathlib import Path
import subprocess
import tempfile
import sys
root = Path(__file__).resolve().parents[1]
generated = Path(sys.argv[1]) if len(sys.argv) > 1 else Path((root / '.local/headless-cache').read_text().strip()) / 'native-local/headless'
source = (generated / 'vulkan_scheduler.cpp').read_text()
header = (generated / 'include/video_core/renderer_vulkan/vk_scheduler.h').read_text()
assert 'std::condition_variable drain_cv;' in header and 'unsigned drain_waiters{};' in header

def method(name):
    start = source.index('void Scheduler::' + name + '(')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
code = r'''
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>
using namespace std::chrono_literals;
inline bool handoff_enabled = true;
extern "C" bool eden_vulkan_handoff_enabled() { return handoff_enabled; }
struct MkwWorkerTimer {};
namespace VideoCommon {
namespace LoadingProbe { struct Probe { void Mark(const char*) {} }; inline Probe gpu; }
namespace GpuSplit { inline bool check_owner{}; inline void CheckOwner(const char*) {} }
namespace Nx2Profile {
inline bool Enabled() { return false; }
inline unsigned long long Now() { return 0; }
enum class Zone { WorkerIdle };
struct Scope { explicit Scope(Zone) {} };
inline void Report(const char*) {}
}
namespace StutterTrace {
enum class Thread { Worker };
struct ThreadScope { explicit ThreadScope(Thread) {} };
}
}
namespace Common {
enum class ThreadPriority { Critical };
namespace DkbCpuTrace {
enum class Role { VulkanWorker };
struct Registration { explicit Registration(Role) {} };
}
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
void SetCurrentThreadToPerformanceCores() {}
}
struct Event : std::condition_variable_any {
    unsigned notifications{};
    void notify_one() { ++notifications; std::condition_variable_any::notify_one(); }
};
using u32 = unsigned;
struct Drain : std::condition_variable {
    std::atomic<unsigned> notifications{};
    void notify_all() { ++notifications; std::condition_variable::notify_all(); }
};
struct CommandChunk {
    std::function<void()> action;
    bool Empty() { return !action; }
    bool HasSubmit() { return false; }
    void ExecuteAll(int, int) { action(); action = {}; }
};
struct Scheduler {
    std::unique_ptr<CommandChunk> chunk = std::make_unique<CommandChunk>();
    std::queue<std::unique_ptr<CommandChunk>> work_queue;
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;
    std::mutex execution_mutex, reserve_mutex, queue_mutex;
    Event event_cv;
    Drain drain_cv;
    unsigned drain_waiters{};
    bool worker_waiting{};
    std::atomic<unsigned> queued_chunks{};
    int current_cmdbuf{}, current_upload_cmdbuf{};
    std::jthread worker_thread;
    Scheduler() : worker_thread([this](std::stop_token stop) { WorkerThread(stop); }) {}
    void WaitWorker();
    void DispatchWork();
    void WorkerThread(std::stop_token);
    void AcquireNewChunk() { chunk = std::make_unique<CommandChunk>(); }
    void AllocateWorkerCommandBuffer() {}
};
'''
code += '\n'.join(method(name) for name in ['WaitWorker', 'DispatchWork', 'WorkerThread'])
code += r'''
int main() {
    for (unsigned mode = 0; mode < 2; ++mode) {
    handoff_enabled = mode;
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        Scheduler scheduler;
        std::binary_semaphore entered{0}, release{0}, final_entered{0}, final_release{0};
        scheduler.chunk->action = [&] { entered.release(); release.acquire(); };
        scheduler.DispatchWork();
        entered.acquire();
        unsigned completed{};
        constexpr unsigned count = 3000;
        for (unsigned i = 0; i < count; ++i) {
            scheduler.chunk->action = [&, i] { assert(completed == i); ++completed; };
            scheduler.DispatchWork();
        }
        scheduler.chunk->action = [&] { final_entered.release(); final_release.acquire(); };
        scheduler.DispatchWork();
        auto drain = std::async(std::launch::async, [&] { scheduler.WaitWorker(); });
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            { std::lock_guard lock{scheduler.queue_mutex}; if (scheduler.drain_waiters) break; }
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        assert(drain.wait_for(5ms) == std::future_status::timeout);
        release.release();
        final_entered.acquire();
        // The queue is empty but the last command is still executing.
        assert(drain.wait_for(5ms) == std::future_status::timeout);
        final_release.release();
        assert(drain.wait_for(5s) == std::future_status::ready);
        drain.get();
        assert(completed == count);
        assert(scheduler.drain_cv.notifications == (mode ? 1u : count + 2));
        scheduler.WaitWorker(); // already empty: no notification required
        scheduler.worker_thread.request_stop();
        scheduler.worker_thread.join(); // blocked worker's stop-aware wait must wake
    }
    }
}
'''
with tempfile.TemporaryDirectory(prefix='vulkan-handoff-') as temp:
    p = Path(temp)
    (p / 'check.cpp').write_text(code)
    subprocess.run(['clang++-18', '-std=c++20', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', str(p / 'check.cpp'), '-o', str(p / 'check')], check=True)
    subprocess.run([str(p / 'check')], check=True, timeout=30)
print('Vulkan handoff PASS: 120000 FIFO commands in both notification modes, queue drain versus execution, one drain wake per burst, empty waits and stopped-worker wake')
