#include <rex/system/gpu_write_signal.h>
#include <rex/cs_scheduler.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace rex::system {
namespace {

std::atomic<uint32_t> g_sequence{0};
// Waiters announce themselves before checking the sequence and the signal
// checks for them after bumping it (both sequentially consistent), so either
// the waiter sees the new sequence or the signal sees the waiter.
std::atomic<uint32_t> g_waiters{0};
std::mutex g_mutex;
std::condition_variable g_condition;

}  // namespace

uint32_t GpuWriteSequence() { return g_sequence.load(); }

void SignalGpuWrite() {
  g_sequence.fetch_add(1);
  if (g_waiters.load()) {
    // A waiter between its check and its wait holds the mutex.
    { std::lock_guard<std::mutex> lock(g_mutex); }
    g_condition.notify_all();
  }
}

void WaitForGpuWrite(uint32_t seen, std::chrono::microseconds timeout) {
  if (cs_scheduler::InActor()) {
    const auto start=cs_scheduler::Now();
    const auto ns=uint64_t(timeout.count())*1000;
    while (g_sequence.load()==seen && cs_scheduler::Now()-start<ns)
      cs_scheduler::Point(seen,cs_scheduler::PointKind::kWait,true,100000);
    return;
  }
  std::unique_lock<std::mutex> lock(g_mutex);
  g_waiters.fetch_add(1);
  g_condition.wait_for(lock, timeout, [seen] { return g_sequence.load() != seen; });
  g_waiters.fetch_sub(1);
}

}  // namespace rex::system
