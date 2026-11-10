#!/usr/bin/env python3
"""Check generated GPU publication, ordering and cancellation without a consumer."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
cache=Path((root/'.local/headless-cache').read_text().strip())
# The fixture compiles against the port's own command queue (a core patch). A core that does not
# ship common/host_command_queue.h has its own queue, and this check has nothing to exercise.
if not (cache/'source/src/common/host_command_queue.h').exists():
    print('GPU producer stop: skipped, the core has no common/host_command_queue.h')
    raise SystemExit(0)
worker=(cache/'native-local/headless/gpu_thread.cpp').read_text()
start=worker.index('u64 ThreadManager::PushCommand(')
body=worker[start:worker.index('\n} // namespace',start)]
assert 'state.queue.EmplaceWait(' not in body
signal_start=worker.index('void SignalCommand(')
signal_body=worker[signal_start:worker.index('\n} // Anonymous namespace',signal_start)]
main=(root/'headless/main.cpp').read_text()
assert 'system.Pause();' not in main
code=r'''
#include <array>
#include <variant>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include "common/host_command_queue.h"
#include "common/host_command_completion.h"
using u64=unsigned long long;
struct CpuReadCommand { struct Diagnostic { unsigned before_lock{}, wait_begin{}, wait_end{}; }* diagnostic{}; };
using CommandData=std::variant<int,CpuReadCommand>;
namespace ReadbackDependencyDiagnostics { inline unsigned Now() { return 0; } }
namespace VideoCommon { struct GpuSplit { static GpuSplit& Get() { static GpuSplit split; return split; } void Stop() {} }; }
struct Item { CommandData command{}; u64 fence{}; std::shared_ptr<Common::HostCommandCompletion> completion; u64 sequence{}; };
struct SynchState { std::mutex write_lock, fence_mutex; u64 last_fence{}; std::atomic<u64> signaled_fence{},published_work_sequence{}; bool shutting_down{};
          Common::HostCommandQueue<Item> queue; std::condition_variable_any fence_cv; };
struct ThreadManager {
 SynchState state;
 std::stop_source stop_source;
 u64 PushCommand(CommandData&&,bool,bool);
 // The worker file defines these after PushCommand; here there is no worker to end.
 void NotifyShutdown();
 void InvalidateReadbackCache() {}
 void Stop() { stop_source.request_stop(); }
};
// headless/CMakeLists.txt times a push that found the queue full (gpu_queue_full), so the body
// below carries Eden's Timer. This standalone TU does not include headless/performance.h - whose
// clock is an extern the test would have to link - so the Timer is stubbed here: two atomics and
// a clock read on scope exit cannot change what the queue does.
namespace Eden::Performance {
struct Totals { std::atomic<unsigned long long> calls{}, nanoseconds{}, requested_bytes{}; };
inline Totals gpu_queue_full;
class Timer {
public:
    explicit Timer(Totals&, unsigned long long = 0) {}
};
} // namespace Eden::Performance
'''+signal_body+'\n'+body+r'''
int main() {
 ThreadManager manager;
 // Publication must exceed the old ring capacity without waiting for a consumer
 // which may itself be waiting for a producer-held registry lock.
 constexpr int count=16384;
 for(int i=0;i<count;++i) assert(manager.PushCommand(int{i},false,true)==u64(i+1));
 Item item;
 for(int i=0;i<count;++i) {
   assert(manager.state.queue.TryPop(item));
   assert(std::get<int>(item.command)==i && item.fence==u64(i+1));
 }
 assert(!manager.state.queue.TryPop(item));
 manager.stop_source.request_stop();
 assert(manager.PushCommand(4,false,true)==0);
 assert(!manager.state.queue.TryPop(item));
 ThreadManager synchronous;
 auto fence_wait=std::async(std::launch::async,[&]{return synchronous.PushCommand(5,true,false);});
 assert(fence_wait.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
 synchronous.stop_source.request_stop();
 assert(fence_wait.wait_for(std::chrono::seconds(1))==std::future_status::ready);
 fence_wait.get();
 // Completion before parking and completion while parked must both publish read results.
 ThreadManager completed;
 auto completing = std::async(std::launch::async,[&]{return completed.PushCommand(6,true,true);});
 Item completion_item;
 while(!completed.state.queue.TryPop(completion_item)) std::this_thread::yield();
 completed.state.signaled_fence.store(100); // An unrelated watermark is not this request's completion.
 assert(completing.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
 SignalCommand(completed.state,completion_item.fence,completion_item.completion);
 assert(completing.get()==1);
 ThreadManager ordered;
 std::array<std::future<u64>,8> waits;
 for(auto& wait:waits) wait=std::async(std::launch::async,[&]{return ordered.PushCommand(7,true,true);});
 std::array<Item,8> items;
 for(auto& pending:items) {
   const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
   while(!ordered.state.queue.TryPop(pending)) {
     assert(std::chrono::steady_clock::now()<deadline);
     std::this_thread::yield();
   }
 }
 for(const auto& pending:items) SignalCommand(ordered.state,pending.fence,pending.completion);
 for(auto& wait:waits) assert(wait.wait_for(std::chrono::seconds(1))==std::future_status::ready);
 for(auto& wait:waits) assert(wait.get()<=8);
 // Cancellation must also work if already requested before the wait registers its callback.
 ThreadManager cancelled;
 cancelled.stop_source.request_stop();
 assert(cancelled.PushCommand(8,true,true)==0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp)/'check.cpp';p.write_text(code);exe=Path(tmp)/'check'
 subprocess.run(['c++','-std=c++20','-pthread','-fsanitize=address,undefined','-I'+str(cache/'source/src'),str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
print('GPU producer: PASS 16384 publications without consumer, FIFO contents/fences, stopped enqueue/fence wait, completion before parking and eight ordered concurrent waiters')