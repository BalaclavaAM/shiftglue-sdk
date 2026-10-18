#pragma once
#include <cstdint>
#include <chrono>
#include <functional>
#include <atomic>
#include <span>
#include <vector>
#include <initializer_list>
#include <rex/runtime_api.h>
namespace rex::thread { class WaitHandle; }

namespace rex::cs_scheduler {
enum class PointKind : uint32_t { kAttach, kCallBudget, kKernel, kWait, kPoll, kGpu, kYield, kExternalReturn };
#if defined(REXGLUE_ENABLE_CS_PROBES)
extern REX_RUNTIME_DATA std::atomic<bool> g_enabled;
inline bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
int Mode();
bool InActor();
uint64_t Now();
uint64_t Epoch();
uint64_t ClockRead(const void* site);
void Point(uint64_t site, PointKind kind, bool force = false, uint64_t delay_ns = 0);
void Call(uint32_t site);
void Detach();
void Sleep(uint64_t ns);
void SetTimestampWriter(std::function<void(uint64_t)> writer);
void Wake(uint64_t tag);
void Signal(thread::WaitHandle* handle);
void MapWait(thread::WaitHandle* original, thread::WaitHandle* replacement);
thread::WaitHandle* ResolveWait(thread::WaitHandle* original);
void ReplayExternalWait();
void Flush();
void Complete();
void Suspend(uint64_t kernel_id);
void Resume(uint64_t kernel_id);
void Terminate(uint64_t kernel_id, const std::function<void()>& stop);
struct Waiting {
  std::vector<thread::WaitHandle*> saved;
  explicit Waiting(std::span<thread::WaitHandle* const> handles);
  Waiting(std::initializer_list<thread::WaitHandle*> handles):Waiting(std::span(handles.begin(),handles.size())) {}
  ~Waiting();
};
struct Actor {
  bool outer = false;
  uint64_t previous_tag = 0;
  explicit Actor(uint64_t tag, uint64_t tick_ns = 0);
  ~Actor();
};
struct Kernel {
  bool active = false;
  explicit Kernel(uint64_t site);
  ~Kernel();
};
struct Callback {
  uint32_t saved;
  Callback();
  ~Callback();
};
#else
inline constexpr bool Enabled() { return false; }
inline constexpr int Mode() { return 0; }
inline constexpr bool InActor() { return false; }
inline uint64_t Now() { return 0; }
inline uint64_t Epoch() { return 0; }
inline uint64_t ClockRead(const void*) { return 0; }
inline void Point(uint64_t, PointKind, bool = false, uint64_t = 0) {}
inline void Call(uint32_t) {}
inline void Detach() {}
inline void Sleep(uint64_t) {}
inline void SetTimestampWriter(std::function<void(uint64_t)>) {}
inline void Wake(uint64_t) {}
inline void Signal(thread::WaitHandle*) {}
inline void MapWait(thread::WaitHandle*,thread::WaitHandle*) {}
inline thread::WaitHandle* ResolveWait(thread::WaitHandle* original) { return original; }
inline void ReplayExternalWait() {}
inline void Flush() {}
inline void Complete() {}
inline void Suspend(uint64_t) {}
inline void Resume(uint64_t) {}
struct Waiting {
  explicit Waiting(std::span<thread::WaitHandle* const>) {}
  Waiting(std::initializer_list<thread::WaitHandle*>) {}
};
struct Actor { explicit Actor(uint64_t, uint64_t = 0) {} };
struct Kernel { explicit Kernel(uint64_t) {} };
struct Callback {};
#endif
inline constexpr uint64_t kMain = UINT64_C(0x8000000000000100);
inline constexpr uint64_t kTimestamp = UINT64_C(0x8000000000000001);
inline constexpr uint64_t kVblank = UINT64_C(0x8000000000000002);
inline constexpr uint64_t kGpu = UINT64_C(0x8000000000000003);
inline constexpr uint64_t kAudio = UINT64_C(0x8000000000000006);

// Native wait arbitration happens with the execution permit held. Polling a
// host event outside the permit could consume another guest's wakeup.
template<typename F>
uint32_t Wait(F&& poll, uint64_t timeout_ns, uint32_t timeout_value = 258) {
  const auto start = Now();
  for (;;) {
    const uint32_t result = poll();
    if (result != timeout_value || timeout_ns == 0) return result;
    if (timeout_ns != UINT64_MAX && Now() - start >= timeout_ns) return timeout_value;
    Point(0, PointKind::kWait, true, 100000);
  }
}
}  // namespace rex::cs_scheduler
