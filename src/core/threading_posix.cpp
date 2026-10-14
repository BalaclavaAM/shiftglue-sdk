/**
 * @file        core/threading_posix.cpp
 * @brief       POSIX platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <deque>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>
#include <semaphore.h>

#if defined(__APPLE__)
#include <cstdio>

#include <fcntl.h>
#endif
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/thread/timer_queue.h>

#include <sched.h>

#include <algorithm>
#include <sys/resource.h>

#if REX_PLATFORM_LINUX
#include <dirent.h>

#include <cstdlib>
#include <sstream>
#include <thread>
#include <unordered_map>
#endif

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>

#include <rex/main_android.h>
#include <rex/string.h>

REXCVAR_DEFINE_STRING(android_thread_cores, "auto", "Kernel",
                      "Pins this process's threads to cores by thread name. auto puts the "
                      "GPU command decoder on the fastest cores where two or more of them "
                      "form their own cluster (Snapdragon 8 Elite); otherwise entries "
                      "NAME=CORES separated by semicolons, NAME a thread-name prefix (* for "
                      "every thread no other entry names; _ also matches a space), CORES a "
                      "list such as 6-7 or 0,2-5. Applies to the threads running when it is "
                      "set; a value without entries (empty, off) restores them")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#elif REX_PLATFORM_LINUX
// NEAR_NATIVE_RESEARCH.md NN-3.14: the same placement by thread name on desktop
// Linux; auto applies the layout measured on the Steam Deck there only.
REXCVAR_DEFINE_STRING(linux_thread_cores, "auto", "Kernel",
                      "Pins this process's threads to cores by thread name. auto applies the "
                      "measured Steam Deck layout on a Steam Deck (LCD or OLED) and changes "
                      "nothing elsewhere; otherwise entries NAME=CORES separated by "
                      "semicolons, NAME a thread-name prefix (* for every thread no other "
                      "entry names; _ also matches a space), CORES a list such as 6-7 or "
                      "0,2-5. Applies to the threads running when it is set; a value without "
                      "entries (empty, off) restores them")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif

REXCVAR_DEFINE_BOOL(posix_event_driven_waits, true, "Kernel",
                    "Wake multi-object and alertable waits when an object is signalled or a "
                    "user callback is queued, instead of polling every millisecond (POSIX only)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Robust mutexes let a wait recover when the owner dies; bionic has none.
#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
#define REX_HAS_ROBUST_MUTEX 1
#else
#define REX_HAS_ROBUST_MUTEX 0
#endif

#if REX_PLATFORM_LINUX
// SIGEV_THREAD_ID in timer_create(...) is a Linux extension
#define REX_HAS_SIGEV_THREAD_ID 1
#ifdef __GLIBC__
#define sigev_notify_thread_id _sigev_un._tid
#endif
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#define gettid() syscall(SYS_gettid)
#endif
#else
#define REX_HAS_SIGEV_THREAD_ID 0
#endif

namespace rex::thread {

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* android_libc_;
// API 26+.
static int (*android_pthread_getname_np_)(pthread_t pthread, char* buf, size_t n);
#endif

#if REX_PLATFORM_LINUX
namespace {
// "6-7" or "0,2-5" as a CPU mask; zero when malformed.
uint64_t ParseCoreList(const std::string& list) {
  uint64_t mask = 0;
  std::stringstream parts(list);
  for (std::string part; std::getline(parts, part, ',');) {
    const size_t dash = part.find('-');
    char* end = nullptr;
    const unsigned long first = std::strtoul(part.c_str(), &end, 10);
    unsigned long last = first;
    if (dash != std::string::npos) last = std::strtoul(part.c_str() + dash + 1, &end, 10);
    if (end == part.c_str() || first > last || last > 63) return 0;
    for (unsigned long cpu = first; cpu <= last; cpu++) mask |= uint64_t(1) << cpu;
  }
  return mask;
}

cpu_set_t MaskToSet(uint64_t mask) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu = 0; cpu < 64; cpu++) {
    if (mask & (uint64_t(1) << cpu)) CPU_SET(cpu, &set);
  }
  return set;
}

// android_thread_cores parsed: a mask per thread-name prefix, and one for
// every other thread (zero leaves them alone).
struct ThreadCoreRules {
  std::string value;
  std::vector<std::pair<std::string, uint64_t>> entries;
  uint64_t others = 0;
};

#if REX_PLATFORM_ANDROID
// "auto": where the fastest cores form a cluster of two or more that is not
// every core (Snapdragon 8 Elite: cores 6-7 at 4.32 GHz, 0-5 at 3.53), the
// GPU command decoder runs on that cluster. Left to the scheduler it spent
// whole stretches on the slower cores: in-run A/B on the 8 Elite, 25 % of
// frames over 17.5 ms at the 60 fps cap and 16.2 ms of decoder CPU a frame,
// against 2 % and 10.2 ms pinned. Elsewhere nothing changes.
std::string AutoThreadCores() {
  std::vector<uint32_t> capacities;
  for (uint32_t cpu = 0; cpu < 64; ++cpu) {
    std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity");
    uint32_t capacity = 0;
    if (!(file >> capacity)) break;
    capacities.push_back(capacity);
  }
  if (capacities.empty()) return {};
  const uint32_t largest = *std::max_element(capacities.begin(), capacities.end());
  std::string top;
  size_t top_count = 0;
  for (size_t cpu = 0; cpu < capacities.size(); ++cpu) {
    if (capacities[cpu] == largest) {
      top += (top.empty() ? "" : ",") + std::to_string(cpu);
      top_count++;
    }
  }
  if (top_count < 2 || top_count == capacities.size()) return {};
  return "GPU_Commands=" + top;
}
#else
std::string ReadFirstLine(const std::string& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

// The Steam Deck's APU: board vendor Valve with product Jupiter (LCD) or
// Galileo (OLED), or the CPU model AMD Custom APU 0405 (LCD) or 0932 (OLED).
// Returns a description of the match, or empty elsewhere.
std::string DetectSteamDeck() {
  const std::string vendor = ReadFirstLine("/sys/devices/virtual/dmi/id/board_vendor");
  const std::string product = ReadFirstLine("/sys/devices/virtual/dmi/id/product_name");
  if (vendor == "Valve" && (product == "Jupiter" || product == "Galileo")) {
    return vendor + " " + product;
  }
  std::ifstream cpuinfo("/proc/cpuinfo");
  for (std::string line; std::getline(cpuinfo, line);) {
    if (line.rfind("model name", 0) != 0) continue;
    if (line.find("AMD Custom APU 0405") != std::string::npos ||
        line.find("AMD Custom APU 0932") != std::string::npos) {
      return line.substr(line.find(':') + 2);
    }
    break;
  }
  return {};
}

// "auto": on a Steam Deck (4 cores, 8 threads, SMT siblings numbered 0-1,
// 2-3, 4-5, 6-7) the simulation and audio threads share core 0's two
// siblings, render and GPU Submission core 1's, GPU Commands and GPU Recorder
// have cores 2 and 3, and every other thread runs on the remaining siblings
// of cores 2 and 3. In-run A/B on the Deck LCD (fh1-race-sustained, Steam
// launches, five runs in two toggle orders): process CPU -6.1 and -6.8 %
// adjusted for draws, from GPU Commands (-7 to -10 %) and the recorder (-6
// to -8 %), p99 +0.08 ms. The guest entries name the base title's thread
// entries and then the v4 title update's (825CEC88, 83075D18, 8261E318);
// only the base layout was measured. Elsewhere nothing changes. Decided once
// and logged once.
std::string AutoThreadCores() {
  static const std::string layout = [] {
    const std::string deck = DetectSteamDeck();
    if (deck.empty()) {
      REXSYS_INFO("linux_thread_cores auto: not a Steam Deck, threads left to the scheduler");
      return std::string();
    }
    for (int core = 0; core < 4; ++core) {
      const std::string expected = std::to_string(core * 2) + "-" + std::to_string(core * 2 + 1);
      const std::string siblings = ReadFirstLine("/sys/devices/system/cpu/cpu" +
                                                 std::to_string(core * 2) +
                                                 "/topology/thread_siblings_list");
      if (siblings != expected) {
        REXSYS_INFO(
            "linux_thread_cores auto: Steam Deck ({}) but cpu{} siblings are '{}', not '{}'; "
            "threads left to the scheduler",
            deck, core * 2, siblings, expected);
        return std::string();
      }
    }
    std::string value =
        "Guest_8255AE10=0;Guest_825CEC88=0;Guest_82FB4AF8=1;Guest_83075D18=1;"
        "Guest_825A6320=2;Guest_8261E318=2;GPU_Submission=3;GPU_Commands=4;GPU_Recorder=6;"
        "*=5,7";
    REXSYS_INFO("linux_thread_cores auto: Steam Deck ({}), layout {}", deck, value);
    return value;
  }();
  return layout;
}
#endif

ThreadCoreRules ParseThreadCoreRules(const std::string& value) {
  ThreadCoreRules rules;
  rules.value = value;
  std::stringstream items(value == "auto" ? AutoThreadCores() : value);
  for (std::string item; std::getline(items, item, ';');) {
    const size_t equals = item.rfind('=');
    if (equals == std::string::npos || equals == 0) continue;
    const uint64_t mask = ParseCoreList(item.substr(equals + 1));
    if (!mask) {
      REXSYS_WARN("thread_cores: bad core list in '{}'", item);
      continue;
    }
    const std::string name = item.substr(0, equals);
    if (name == "*") {
      rules.others = mask;
    } else {
      rules.entries.emplace_back(name, mask);
    }
  }
  return rules;
}

uint64_t ThreadCoreMask(const ThreadCoreRules& rules, std::string_view name) {
  for (const auto& [prefix, mask] : rules.entries) {
    if (name.size() >= prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), name.begin(),
                   [](char p, char c) { return p == c || (p == '_' && c == ' '); })) {
      return mask;
    }
  }
  return rules.others;
}

std::mutex android_thread_cores_mutex_;
ThreadCoreRules android_thread_cores_rules_;
// The masks threads had before android_thread_cores first changed them.
std::unordered_map<pid_t, cpu_set_t> android_thread_cores_saved_;

// Pins `tid` to `mask`, or restores its earlier mask when `mask` is zero.
// Returns 1 when pinned, -1 when restored, 0 otherwise. Caller holds the mutex.
int PinThreadLocked(pid_t tid, uint64_t mask) {
  auto saved = android_thread_cores_saved_.find(tid);
  if (mask) {
    if (saved == android_thread_cores_saved_.end()) {
      cpu_set_t original;
      if (sched_getaffinity(tid, sizeof(original), &original) != 0) return 0;
      android_thread_cores_saved_.emplace(tid, original);
    }
    const cpu_set_t set = MaskToSet(mask);
    return sched_setaffinity(tid, sizeof(set), &set) == 0 ? 1 : 0;
  }
  if (saved != android_thread_cores_saved_.end()) {
    sched_setaffinity(tid, sizeof(saved->second), &saved->second);
    android_thread_cores_saved_.erase(saved);
    return -1;
  }
  return 0;
}

void ApplyAndroidThreadCores(const std::string& value) {
  std::lock_guard lock(android_thread_cores_mutex_);
  android_thread_cores_rules_ = ParseThreadCoreRules(value);
  DIR* tasks = opendir("/proc/self/task");
  if (!tasks) return;
  size_t pinned = 0, restored = 0;
  while (const dirent* entry = readdir(tasks)) {
    const pid_t tid = pid_t(std::atoi(entry->d_name));
    if (tid <= 0) continue;
    std::ifstream comm_file("/proc/self/task/" + std::string(entry->d_name) + "/comm");
    std::string comm;
    std::getline(comm_file, comm);
    const int result = PinThreadLocked(tid, ThreadCoreMask(android_thread_cores_rules_, comm));
    pinned += result > 0;
    restored += result < 0;
  }
  closedir(tasks);
  REXSYS_INFO("thread_cores '{}': {} threads pinned, {} restored", value, pinned,
              restored);
}

#if !REX_PLATFORM_ANDROID
// Set by an exit handler, under the mutex, so the sweep below never touches
// the rules or the cvars while static destructors run.
bool linux_thread_cores_stopping_ = false;

// A layout set at startup (auto on a Deck) has to place the threads nobody
// names (the main thread, the driver's, SDL's and PipeWire's), and threads
// inherit the mask of the thread that starts them: SDL's audio threads,
// started from the simulation thread, would share its core. The measured
// layout had every thread placed by its name, so while that value stays,
// every thread whose mask differs from its name's is placed again every 5 s.
void SweepThreadCores(std::string value) {
  ApplyAndroidThreadCores(value);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::lock_guard lock(android_thread_cores_mutex_);
    if (linux_thread_cores_stopping_ || REXCVAR_GET(linux_thread_cores) != value) return;
    if (android_thread_cores_rules_.value != value) {
      android_thread_cores_rules_ = ParseThreadCoreRules(value);
    }
    DIR* tasks = opendir("/proc/self/task");
    if (!tasks) return;
    size_t moved = 0;
    while (const dirent* entry = readdir(tasks)) {
      const pid_t tid = pid_t(std::atoi(entry->d_name));
      if (tid <= 0) continue;
      std::ifstream comm_file("/proc/self/task/" + std::string(entry->d_name) + "/comm");
      std::string comm;
      std::getline(comm_file, comm);
      const uint64_t mask = ThreadCoreMask(android_thread_cores_rules_, comm);
      cpu_set_t current;
      if (!mask || sched_getaffinity(tid, sizeof(current), &current) != 0) continue;
      const cpu_set_t wanted = MaskToSet(mask);
      if (CPU_EQUAL(&current, &wanted)) continue;
      moved += PinThreadLocked(tid, mask) > 0;
    }
    closedir(tasks);
    if (moved) REXSYS_INFO("thread_cores '{}': {} more threads placed", value, moved);
  }
}
#endif
}  // namespace

// Pins a thread as android_thread_cores asks for its new name; true when it
// did (the thread then keeps that mask over other placement).
bool PinAndroidNamedThread(pid_t tid, std::string_view name) {
#if !REX_PLATFORM_ANDROID
  // Desktop Linux has no AndroidInitialize: register the hot-reload callback
  // when the first thread is named, after the cvars are parsed.
  static std::once_flag callback_once;
  std::call_once(callback_once, [] {
    rex::cvar::RegisterChangeCallback("linux_thread_cores",
                                      [](std::string_view, std::string_view value) {
                                        std::thread(ApplyAndroidThreadCores, std::string(value))
                                            .detach();
                                      });
    const std::string value = REXCVAR_GET(linux_thread_cores);
    if (!(value == "auto" ? AutoThreadCores() : value).empty()) {
      std::atexit([] {
        std::lock_guard lock(android_thread_cores_mutex_);
        linux_thread_cores_stopping_ = true;
      });
      std::thread(SweepThreadCores, value).detach();
    }
  });
#endif
#if REX_PLATFORM_ANDROID
  const std::string value = REXCVAR_GET(android_thread_cores);
#else
  const std::string value = REXCVAR_GET(linux_thread_cores);
#endif
  std::lock_guard lock(android_thread_cores_mutex_);
  if (android_thread_cores_rules_.value != value) {
    android_thread_cores_rules_ = ParseThreadCoreRules(value);
  }
  const uint64_t mask = ThreadCoreMask(android_thread_cores_rules_, name);
  if (PinThreadLocked(tid, mask) <= 0) return false;
  // Threads are created unnamed and named right after; log the name only.
  if (!name.empty()) REXSYS_INFO("thread_cores: thread '{}' on cores {:#x}", name, mask);
  return true;
}
#endif

#if REX_PLATFORM_ANDROID
void AndroidInitialize() {
  rex::cvar::RegisterChangeCallback("android_thread_cores",
                                    [](std::string_view, std::string_view value) {
                                      // Off the registry lock the callback runs under.
                                      std::thread(ApplyAndroidThreadCores, std::string(value))
                                          .detach();
                                    });
  if (rex::GetAndroidApiLevel() >= 26) {
    android_libc_ = dlopen("libc.so", RTLD_NOW);
    assert_not_null(android_libc_);
    if (android_libc_) {
      android_pthread_getname_np_ = reinterpret_cast<decltype(android_pthread_getname_np_)>(
          dlsym(android_libc_, "pthread_getname_np"));
      assert_not_null(android_pthread_getname_np_);
    }
  }
}

void AndroidShutdown() {
  android_pthread_getname_np_ = nullptr;
  if (android_libc_) {
    dlclose(android_libc_);
    android_libc_ = nullptr;
  }
}
#endif

template <typename _Rep, typename _Period>
inline timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Thread interruption is done using user-defined signals
// This implementation uses the SIGRTMAX - SIGRTMIN to signal to a thread
// gdb tip, for SIG = SIGRTMIN + SignalType : handle SIG nostop
// lldb tip, for SIG = SIGRTMIN + SignalType : process handle SIG -s false
enum class SignalType {
  kThreadSuspend,
  kThreadUserCallback,
#if REX_PLATFORM_ANDROID
  // pthread_cancel is not available on Android, using a signal handler for
  // simplified PTHREAD_CANCEL_ASYNCHRONOUS-like behavior - not disabling
  // cancellation currently, so should be enough.
  kThreadTerminate,
#endif
  k_Count
};

int GetSystemSignal(SignalType num) {
#if defined(__APPLE__)
  // macOS has no realtime signals (SIGRTMIN..SIGRTMAX); map the runtime's
  // signals onto the two user-defined signals.
  switch (num) {
    case SignalType::kThreadSuspend:
      return SIGUSR1;
    case SignalType::kThreadUserCallback:
      return SIGUSR2;
    default:
      assert_unhandled_case(num);
      return SIGUSR1;
  }
#else
  auto result = SIGRTMIN + static_cast<int>(num);
  assert_true(result < SIGRTMAX);
  return result;
#endif
}

SignalType GetSystemSignalType(int num) {
#if defined(__APPLE__)
  switch (num) {
    case SIGUSR1:
      return SignalType::kThreadSuspend;
    case SIGUSR2:
      return SignalType::kThreadUserCallback;
    default:
      assert_unhandled_case(num);
      return SignalType::kThreadSuspend;
  }
#else
  return static_cast<SignalType>(num - SIGRTMIN);
#endif
}

std::array<std::atomic<bool>, static_cast<size_t>(SignalType::k_Count)> signal_handler_installed =
    {};

static void signal_handler(int signal, siginfo_t* info, void* context);

void install_signal_handler(SignalType type) {
  bool expected = false;
  if (!signal_handler_installed[static_cast<size_t>(type)].compare_exchange_strong(expected,
                                                                                   true)) {
    return;
  }
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {
    signal_handler_installed[static_cast<size_t>(type)] = false;
  }
}

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// The core topology is not read on POSIX yet; callers fall back to one
// logical processor per guest hardware thread.
uint64_t GuestCpuAffinityMask(uint8_t) {
  return 0;
}

#if !defined(__APPLE__)
namespace {
// The cores whose capacity (the kernel's relative performance, 1024 for the
// fastest) is at least half the largest: on a phone with prime, big and
// little clusters (Snapdragon 8 Gen 2: 1 + 4 + 3), the prime and big cores.
// Zero when every core is alike or the kernel does not say.
uint64_t PerformanceCoreMask() {
  static const uint64_t mask = [] {
    std::vector<uint32_t> capacities;
    for (uint32_t cpu = 0; cpu < 64; ++cpu) {
      std::ifstream file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpu_capacity");
      uint32_t capacity = 0;
      if (!(file >> capacity)) {
        break;
      }
      capacities.push_back(capacity);
    }
    if (capacities.empty()) {
      return uint64_t(0);
    }
    const auto [smallest, largest] = std::minmax_element(capacities.begin(), capacities.end());
    std::string listing;
    for (uint32_t capacity : capacities) {
      listing += (listing.empty() ? "" : " ") + std::to_string(capacity);
    }
    if (*smallest == *largest) {
      REXSYS_INFO("CPU capacities: {} (all alike, no core preference)", listing);
      return uint64_t(0);
    }
    uint64_t result = 0;
    for (size_t cpu = 0; cpu < capacities.size(); ++cpu) {
      if (capacities[cpu] * 2 >= *largest) {
        result |= uint64_t(1) << cpu;
      }
    }
    REXSYS_INFO("CPU capacities: {}; latency-critical threads on cores {:#x}", listing, result);
    return result;
  }();
  return mask;
}
}  // namespace
#endif

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
#if defined(__APPLE__)
  return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#else
  return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

void MaybeYield() {
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() {
  __sync_synchronize();
}

void SleepUntil(std::chrono::steady_clock::time_point deadline, std::chrono::microseconds spin) {
  using Clock = std::chrono::steady_clock;
  if (Clock::now() < deadline - spin) std::this_thread::sleep_until(deadline - spin);
  while (Clock::now() < deadline) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    sched_yield();
#endif
  }
}

void WaitUntil(std::chrono::steady_clock::time_point deadline) {
  std::this_thread::sleep_until(deadline);
}

void Sleep(std::chrono::microseconds duration) {
  timespec rqtp = DurationToTimeSpec(duration);
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  do {
    ret = nanosleep(p_rqtp, p_rmtp);
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
bool DispatchCurrentThreadUserCallback();

// What a thread sleeps on while it waits on several conditions, or alertably
// on any: each condition it waits on, and a user callback queued to it, bumps
// the generation, so the wait wakes on the signal. It replaces 1 ms polling,
// which cost a wake-up per millisecond and up to 1 ms of latency per wait.
class PosixWaiter {
 public:
  uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

  void Notify() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      generation_.fetch_add(1, std::memory_order_release);
    }
    cond_.notify_one();
  }

  // Sleeps until the generation differs from `seen` or the deadline passes.
  void WaitForChange(uint64_t seen, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto changed = [&] { return generation_.load(std::memory_order_relaxed) != seen; };
    if (deadline == std::chrono::steady_clock::time_point::max()) {
      cond_.wait(lock, changed);
    } else {
      cond_.wait_until(lock, deadline, changed);
    }
  }

 private:
  std::mutex mutex_;
  std::condition_variable cond_;
  std::atomic<uint64_t> generation_{0};
};

// The waiter of the calling thread; a user callback queued to a thread
// notifies the same one.
PosixWaiter& CurrentThreadWaiter();

SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  auto deadline = std::chrono::steady_clock::now() + duration;
  const bool event_driven = REXCVAR_GET(posix_event_driven_waits);
  while (true) {
    const uint64_t seen = event_driven ? CurrentThreadWaiter().generation() : 0;
    if (DispatchCurrentThreadUserCallback()) {
      alertable_state_ = false;
      return SleepResult::kAlerted;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      alertable_state_ = false;
      return SleepResult::kSuccess;
    }
    if (event_driven) {
      CurrentThreadWaiter().WaitForChange(seen, deadline);
      continue;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    Sleep(std::min(remaining, std::chrono::microseconds(1000)));
  }
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) {
  return pthread_key_delete(static_cast<pthread_key_t>(handle)) == 0;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(static_cast<pthread_key_t>(handle)));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(static_cast<pthread_key_t>(handle), reinterpret_cast<void*>(value)) ==
         0;
}

#if defined(__APPLE__)
// macOS does not implement unnamed POSIX semaphores (sem_init fails with
// ENOSYS). Emulate one with a named semaphore that is unlinked immediately
// after creation: the name is released but the descriptor stays valid until
// sem_close, and sem_wait/sem_post remain async-signal-safe (needed because
// WaitSuspended() runs from a signal handler).
static sem_t* RexCreateAnonymousSemaphore() {
  static std::atomic<uint64_t> counter{0};
  char name[64];
  std::snprintf(name, sizeof(name), "/rexsusp_%d_%llu", static_cast<int>(getpid()),
                static_cast<unsigned long long>(counter.fetch_add(1, std::memory_order_relaxed)));
  sem_t* sem = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
  if (sem != SEM_FAILED) {
    sem_unlink(name);
  }
  return sem;
}
#endif  // __APPLE__

class PosixConditionBase {
 public:
  PosixConditionBase() {
#if REX_HAS_ROBUST_MUTEX
    // Use robust mutexes so waits can recover if owner thread terminates.
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
      if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0) {
        auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
        pthread_mutex_destroy(native_mutex);
        pthread_mutex_init(native_mutex, &attr);
      }
      pthread_mutexattr_destroy(&attr);
    }
#endif
  }

  virtual ~PosixConditionBase() = default;
  virtual bool Signal() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    bool executed;
    auto predicate = [this] { return this->signaled(); };
#if REX_HAS_ROBUST_MUTEX
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    int lock_result = pthread_mutex_lock(native_mutex);
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (lock_result != 0) {
      return WaitResult::kFailed;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
#else
    std::unique_lock<std::mutex> lock(mutex_);
#endif
    if (predicate()) {
      executed = true;
    } else {
      ++cond_waiters_;
      if (timeout == std::chrono::milliseconds::max()) {
        cond_.wait(lock, predicate);
        executed = true;  // Did not time out;
      } else {
        executed = cond_.wait_for(lock, timeout, predicate);
      }
      --cond_waiters_;
    }
    if (executed) {
      post_execution();
      return WaitResult::kSuccess;
    } else {
      return WaitResult::kTimeout;
    }
  }

  // Waits on the handles, and when `alertable` also for a user callback queued
  // to this thread, by sleeping on the thread's waiter until one of them
  // notifies it.
  static std::pair<WaitResult, size_t> WaitEventDriven(
      const std::vector<PosixConditionBase*>& handles, bool wait_all, bool alertable,
      std::chrono::milliseconds timeout) {
    const auto deadline = (timeout == std::chrono::milliseconds::max())
                              ? std::chrono::steady_clock::time_point::max()
                              : std::chrono::steady_clock::now() + timeout;
    // Before the waiter is chosen: it creates this thread's Thread object, which
    // owns the waiter user callbacks notify.
    if (alertable && DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    PosixWaiter& waiter = CurrentThreadWaiter();
    for (auto* handle : handles) {
      handle->AddWaiter(&waiter);
    }
    std::pair<WaitResult, size_t> result;
    while (true) {
      // Read before the checks, so a signal after a check changes it.
      const uint64_t seen = waiter.generation();
      if (alertable && DispatchCurrentThreadUserCallback()) {
        result = std::make_pair(WaitResult::kUserCallback, 0);
        break;
      }
      size_t index = 0;
      if (TryAcquire(handles, wait_all, &index)) {
        result = std::make_pair(WaitResult::kSuccess, index);
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        result = std::make_pair(WaitResult::kTimeout, 0);
        break;
      }
      waiter.WaitForChange(seen, deadline);
    }
    for (auto* handle : handles) {
      handle->RemoveWaiter(&waiter);
    }
    return result;
  }

  static std::pair<WaitResult, size_t> WaitMultiple(std::vector<PosixConditionBase*>&& handles,
                                                    bool wait_all,
                                                    std::chrono::milliseconds timeout) {
    assert_true(!handles.empty());

    if (handles.size() == 1) {
      auto result = handles[0]->Wait(timeout);
      return std::make_pair(result, 0);
    }
    if (REXCVAR_GET(posix_event_driven_waits)) {
      return WaitEventDriven(handles, wait_all, false, timeout);
    }

    auto start_time = std::chrono::steady_clock::now();
    auto end_time = (timeout == std::chrono::milliseconds::max())
                        ? std::chrono::steady_clock::time_point::max()
                        : start_time + timeout;

    while (true) {
      size_t first_signaled = std::numeric_limits<size_t>::max();
      bool condition_met = false;
      bool all_locked = true;

      std::vector<std::unique_lock<std::mutex>> locks;
      locks.reserve(handles.size());

      for (size_t i = 0; i < handles.size(); ++i) {
#if REX_HAS_ROBUST_MUTEX
        auto native_mutex = static_cast<pthread_mutex_t*>(handles[i]->mutex_.native_handle());
        int result = pthread_mutex_trylock(native_mutex);
        if (result == 0 || result == EOWNERDEAD) {
          if (result == EOWNERDEAD) {
            pthread_mutex_consistent(native_mutex);
          }
          locks.emplace_back(handles[i]->mutex_, std::adopt_lock);
        } else {
          all_locked = false;
          break;
        }
#else
        locks.emplace_back(handles[i]->mutex_, std::try_to_lock);
        if (!locks.back().owns_lock()) {
          all_locked = false;
          break;
        }
#endif
      }

      if (!all_locked) {
        locks.clear();
        std::this_thread::yield();
        continue;
      }

      if (wait_all) {
        bool all_signaled = true;
        for (size_t i = 0; i < handles.size(); ++i) {
          if (!handles[i]->signaled()) {
            all_signaled = false;
            break;
          }
          if (first_signaled == std::numeric_limits<size_t>::max()) {
            first_signaled = i;
          }
        }
        condition_met = all_signaled;
      } else {
        for (size_t i = 0; i < handles.size(); ++i) {
          if (handles[i]->signaled()) {
            first_signaled = i;
            condition_met = true;
            break;
          }
        }
      }

      if (condition_met) {
        if (wait_all) {
          for (size_t i = 0; i < handles.size(); ++i) {
            handles[i]->post_execution();
          }
        } else {
          handles[first_signaled]->post_execution();
        }
        return std::make_pair(WaitResult::kSuccess, first_signaled);
      }

      locks.clear();

      auto now = std::chrono::steady_clock::now();
      if (now >= end_time) {
        return std::make_pair<WaitResult, size_t>(WaitResult::kTimeout, 0);
      }

      if (timeout == std::chrono::milliseconds::max()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        // At the clock's own resolution: truncated to milliseconds, the last
        // millisecond of every wait slept for 0 and spun, and alertable waits
        // (1 ms slices) spun for their whole length; the audio worker took
        // three quarters of a phone core that way.
        const auto remaining = end_time - now;
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
            remaining, std::chrono::milliseconds(1)));
      }
    }
  }

  virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;

  // Wakes single waits and the waiters of multi-object and alertable waits.
  // Called with mutex_ held, after the state change.
  void NotifyAllLocked() {
    PERF_counter_inc(kKernelObjectNotifies);
    if (!cond_waiters_ && waiters_.empty()) {
      PERF_counter_inc(kKernelObjectNotifiesNoWaiter);
    }
    cond_.notify_all();
    for (auto* waiter : waiters_) {
      waiter->Notify();
    }
  }

  std::condition_variable cond_;
  std::mutex mutex_;
  // Single-object waits blocked on cond_ (under mutex_), so a notify can tell
  // whether anyone was waiting (kKernelObjectNotifiesNoWaiter, NN-3.3).
  uint32_t cond_waiters_ = 0;

 private:
  std::unique_lock<std::mutex> Lock() {
#if REX_HAS_ROBUST_MUTEX
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
    int result = pthread_mutex_lock(native_mutex);
    if (result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
    } else if (result != 0) {
      return {};
    }
    return std::unique_lock<std::mutex>(mutex_, std::adopt_lock);
#else
    return std::unique_lock<std::mutex>(mutex_);
#endif
  }

  void AddWaiter(PosixWaiter* waiter) {
    auto lock = Lock();
    waiters_.push_back(waiter);
  }

  void RemoveWaiter(PosixWaiter* waiter) {
    auto lock = Lock();
    auto it = std::find(waiters_.begin(), waiters_.end(), waiter);
    if (it != waiters_.end()) {
      waiters_.erase(it);
    }
  }

  // Takes the first signalled handle (wait any) or all of them (wait all).
  static bool TryAcquire(const std::vector<PosixConditionBase*>& handles, bool wait_all,
                         size_t* out_index) {
    if (!wait_all) {
      // One handle at a time is enough: a wait-any acquires only one.
      for (size_t i = 0; i < handles.size(); ++i) {
        auto lock = handles[i]->Lock();
        if (lock.owns_lock() && handles[i]->signaled()) {
          handles[i]->post_execution();
          *out_index = i;
          return true;
        }
      }
      return false;
    }
    // Wait all needs every lock at once; taking them in a fixed order cannot
    // deadlock with another wait-all, and signallers hold only one.
    std::vector<PosixConditionBase*> order(handles);
    std::sort(order.begin(), order.end());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(order.size());
    for (auto* handle : order) {
      locks.push_back(handle->Lock());
      if (!locks.back().owns_lock()) {
        return false;
      }
    }
    for (auto* handle : order) {
      if (!handle->signaled()) {
        return false;
      }
    }
    for (auto* handle : order) {
      handle->post_execution();
    }
    *out_index = 0;
    return true;
  }

  // Waiters of multi-object and alertable waits on this condition; mutex_.
  std::vector<PosixWaiter*> waiters_;
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  virtual ~PosixCondition() = default;

  bool Signal() override {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = true;
    NotifyAllLocked();
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = false;
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  bool signal_;
  const bool manual_reset_;
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool Signal() override { return Release(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    if (release_count > maximum_count_ - count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    count_ += release_count;
    NotifyAllLocked();
    return true;
  }

 private:
  inline bool signaled() const override { return count_ > 0; }
  inline void post_execution() override {
    count_--;
    cond_.notify_all();
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool Signal() override { return Release(); }

  bool Release() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      --count_;
      // Free to be acquired by another thread
      if (count_ == 0) {
        NotifyAllLocked();
      }
      return true;
    }
    return false;
  }

  void* native_handle() const override { return const_cast<std::mutex&>(mutex_).native_handle(); }

 private:
  inline bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  inline void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  virtual ~PosixCondition() { Cancel(); }

  bool Signal() override {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = true;
    NotifyAllLocked();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = reinterpret_cast<PosixCondition<Timer>*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {
#if defined(__APPLE__)
    suspend_sem_ = RexCreateAnonymousSemaphore();
#else
    sem_init(&suspend_sem_, 0, 0);
#endif
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params, ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
      return false;
    if (pthread_attr_setstacksize(&attr, params.stack_size) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
#if !REX_PLATFORM_LINUX
    // Linux maps priorities to nice values in set_priority: Android apps may
    // not use SCHED_FIFO, and a desktop process rarely may (no CAP_SYS_NICE),
    // while a guest priority of 0 or below is invalid for it anyway.
    if (params.initial_priority != 0) {
      sched_param sched{};
      sched.sched_priority = params.initial_priority + 1;
      if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
      if (pthread_attr_setschedparam(&attr, &sched) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
    }
#endif
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start_data) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    pthread_attr_destroy(&attr);
    return true;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {
#if defined(__APPLE__)
    suspend_sem_ = RexCreateAnonymousSemaphore();
#else
    sem_init(&suspend_sem_, 0, 0);
#endif
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }

  virtual ~PosixCondition() {
    // Match Canary/Edge behavior.
    // Force-cancel/join from the condition destructor can self-join/crash
    // depending on shutdown ordering, so threads must be stopped explicitly.
  }

  bool Signal() override { return true; }

  std::string name() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if REX_PLATFORM_ANDROID
      // pthread_getname_np was added in API 26 - below that, store the name in
      // this object, which may be only modified through Xenia threading, but
      // should be enough in most cases.
      if (android_pthread_getname_np_) {
        if (android_pthread_getname_np_(thread_, result.data(), result.size() - 1) != 0) {
          assert_always();
        }
      } else {
        std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
        rex::string::copy_truncating(result.data(), android_pre_api_26_name_, result.size());
      }
#else
      if (pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
        assert_always();
      }
#endif
    }
    return std::string(result.data());
  }

  void set_name(const std::string& name) {
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if defined(__APPLE__)
      // macOS pthread_setname_np() only names the calling thread.
      if (pthread_equal(thread_, pthread_self())) {
        pthread_setname_np(std::string(name).c_str());
      }
#else
      pthread_setname_np(thread_, std::string(name).c_str());
#endif
#if REX_PLATFORM_ANDROID
      SetAndroidPreApi26Name(name);
      PinAndroidNamedThread(pthread_gettid_np(thread_), name);
      rex::perf::NoteNamedThread(pthread_gettid_np(thread_), name);
#elif REX_PLATFORM_LINUX
      PinAndroidNamedThread(tid_, name);
      rex::perf::NoteNamedThread(tid_, name);
#endif
    }
  }

#if REX_PLATFORM_ANDROID
  void SetAndroidPreApi26Name(const std::string_view name) {
    if (android_pthread_getname_np_) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    rex::string::copy_truncating(android_pre_api_26_name_, name,
                                 rex::countof(android_pre_api_26_name_));
  }
#endif

#if defined(__APPLE__)
  uint32_t system_id() const { return static_cast<uint32_t>(pthread_mach_thread_np(thread_)); }
#else
  uint32_t system_id() const { return static_cast<uint32_t>(thread_); }
#endif
#if REX_PLATFORM_ANDROID
  pid_t tid() const { return pthread_gettid_np(thread_); }
#elif REX_PLATFORM_LINUX
  pid_t tid() const { return tid_; }
#endif

  uint64_t affinity_mask() {
    WaitStarted();
#if defined(__APPLE__)
    // macOS doesn't expose Linux-style pthread affinity masks; report every
    // logical processor as available.
    auto cpu_count = std::min<uint32_t>(logical_processor_count(), 64);
    if (cpu_count == 64) {
      return std::numeric_limits<uint64_t>::max();
    }
    return (uint64_t(1) << cpu_count) - 1;
#else
    cpu_set_t cpu_set;
#if REX_PLATFORM_ANDROID
    if (sched_getaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_getaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
    uint64_t result = 0;
    auto cpu_count = std::min(CPU_SETSIZE, 64);
    for (auto i = 0u; i < cpu_count; i++) {
      auto set = CPU_ISSET(i, &cpu_set);
      result |= set << i;
    }
    return result;
#endif
  }

  void set_affinity_mask(uint64_t mask) {
    WaitStarted();
#if defined(__APPLE__)
    // macOS doesn't expose Linux-style pthread affinity masks; keep this a
    // no-op so higher layers can build and run.
    (void)mask;
#else
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (auto i = 0u; i < 64; i++) {
      if (mask & (1ULL << i)) {
        CPU_SET(i, &cpu_set);
      }
    }
#if REX_PLATFORM_ANDROID
    if (sched_setaffinity(pthread_gettid_np(thread_), sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#else
    if (pthread_setaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
#endif
  }

  int priority() {
    WaitStarted();
    int policy;
    sched_param param{};
    int ret = pthread_getschedparam(thread_, &policy, &param);
    if (ret != 0) {
      return -1;
    }

    return param.sched_priority;
  }

  void set_priority(int new_priority) {
    WaitStarted();
#if REX_PLATFORM_LINUX
    // Real-time policies need privileges neither an Android app nor a desktop
    // process normally has, so the guest's priorities (THREAD_PRIORITY_*, up
    // to 15 for time critical) map to nice -10 (most urgent) .. +4. Android
    // lets an app lower its threads' nice value; elsewhere raising one needs
    // RLIMIT_NICE or CAP_SYS_NICE, and without them the thread stays at 0.
    const int nice = std::clamp(-2 * new_priority, -10, 4);
#if REX_PLATFORM_ANDROID
    const pid_t tid = pthread_gettid_np(thread_);
#else
    const pid_t tid = tid_;
#endif
    if (setpriority(PRIO_PROCESS, tid, nice) != 0) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) {
        REXSYS_WARN("set_priority: setpriority({}) failed ({}); further failures are not logged",
                    nice, errno);
      }
    }
    return;
#endif
    sched_param param{};
    param.sched_priority = new_priority;
    int result = pthread_setschedparam(thread_, SCHED_FIFO, &param);
    if (result != 0) {
      switch (result) {
        case EPERM:
          REXSYS_WARN("set_priority: permission denied");
          break;
        case EINVAL:
          assert_always();
          break;
        default:
          REXSYS_WARN("set_priority: pthread_setschedparam failed ({})", result);
          break;
      }
    }
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      user_callbacks_.push_back(std::move(callback));
      has_pending_user_callbacks_.store(true, std::memory_order_release);
    }

    // If the callback is queued on the current thread, don't self-signal.
    // Alertable waits drain this queue in normal thread context.
    if (pthread_equal(thread_, pthread_self())) {
      if (alertable_state_) {
        DispatchQueuedUserCallbacks();
      }
      return;
    }

    // A thread that has finished cannot run the callback, and its pthread_t
    // may already be gone: bionic aborts the process on a stale one (every
    // route ended in a crash dump at shutdown). The thread marks itself
    // finished under state_mutex_, so it stays alive while this is held.
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    if (state_ == State::kFinished) {
      return;
    }
    // Wakes an event-driven alertable wait; the signal below covers code that
    // drains callbacks from the signal handler.
    waiter_.Notify();
#if defined(__APPLE__)
    // macOS has no sigqueue/pthread_sigqueue; pthread_kill delivers the signal
    // without the sigval payload.
    int result = pthread_kill(thread_, GetSystemSignal(SignalType::kThreadUserCallback));
#else
    sigval value{};
    value.sival_ptr = this;
    // sigqueue takes a process id, so on Android it never reached the thread
    // (ESRCH): every queued APC, the completion routine of an overlapped
    // read among them, was dropped. Bionic has pthread_sigqueue from API 29.
    int result = pthread_sigqueue(thread_, GetSystemSignal(SignalType::kThreadUserCallback), value);
#endif
    if (result != 0) {
      REXSYS_WARN("QueueUserCallback: signal delivery failed ({})", result);
    }
  }

  PosixWaiter& waiter() { return waiter_; }

  bool DispatchQueuedUserCallbacks() {
    if (!has_pending_user_callbacks_.load(std::memory_order_acquire)) {
      return false;
    }

    std::function<void()> callback;
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      if (user_callbacks_.empty()) {
        has_pending_user_callbacks_.store(false, std::memory_order_release);
        return false;
      }
      callback = std::move(user_callbacks_.front());
      user_callbacks_.pop_front();
      has_pending_user_callbacks_.store(!user_callbacks_.empty(), std::memory_order_release);
    }
    if (callback) {
      callback();
    }
    return true;
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0 && state_ == State::kSuspended) {
      state_ = State::kRunning;
      // Async-signal-safe wakeup for WaitSuspended() from signal handler path.
      sem_post(suspend_sem_ptr());
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    if (is_current_thread) {
      WaitSuspended();
      return true;
    }

    int result = pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend));
    return result == 0;
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          // This is really bad. Some thread must have called Terminate() on us
          // just before we decided to terminate ourselves
          assert_always();
          for (;;) {
            // Wait for pthread_cancel() to actually happen.
          }
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);

      exit_code_ = exit_code;
      signaled_ = true;
      NotifyAllLocked();
    }
    if (is_current_thread) {
      pthread_exit(reinterpret_cast<void*>(exit_code));
    } else {
#if REX_PLATFORM_ANDROID
      if (pthread_kill(thread_, GetSystemSignal(SignalType::kThreadTerminate)) != 0) {
        assert_always();
      }
#else
      if (pthread_cancel(thread_) != 0) {
        assert_always();
      }
#endif
    }
  }

  void WaitStarted() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_signal_.wait(lock, [this] { return state_ != State::kUninitialized; });
  }

  /// Uses sem_wait because it may be called from signal handler context.
  void WaitSuspended() {
    int ret;
    do {
      ret = sem_wait(suspend_sem_ptr());
    } while (ret == -1 && errno == EINTR);
  }

  void* native_handle() const override { return reinterpret_cast<void*>(thread_); }

 private:
  static void* ThreadStartRoutine(void* parameter);
  inline bool signaled() const override { return signaled_; }
  inline void post_execution() override {
    // Every satisfied wait on an exited thread lands here (under mutex_);
    // reap it once. Joining twice is undefined, and bionic aborts on it
    // ("invalid pthread_t passed to pthread_join") when a guest waits on the
    // same thread handle again.
    if (reaped_) {
      return;
    }
    reaped_ = true;
    if (thread_) {
      pthread_join(thread_, nullptr);
    }
#if defined(__APPLE__)
    if (suspend_sem_ != SEM_FAILED) {
      sem_close(suspend_sem_);
      suspend_sem_ = SEM_FAILED;
    }
#else
    sem_destroy(&suspend_sem_);
#endif
  }
  pthread_t thread_;
#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
  pid_t tid_ = 0;  // Kernel thread id for setpriority; set before the start signal.
#endif
  bool reaped_ = false;
  bool signaled_;
  int exit_code_;
  State state_;             // Protected by state_mutex_
  uint32_t suspend_count_;  // Protected by state_mutex_
#if defined(__APPLE__)
  sem_t* suspend_sem_ = SEM_FAILED;
  sem_t* suspend_sem_ptr() { return suspend_sem_; }
#else
  sem_t suspend_sem_;
  sem_t* suspend_sem_ptr() { return &suspend_sem_; }
#endif
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::deque<std::function<void()>> user_callbacks_;
  std::atomic<bool> has_pending_user_callbacks_{false};
  PosixWaiter waiter_;  // Waits of this thread; a queued user callback notifies it.
#if REX_PLATFORM_ANDROID
  // Name accessible via name() on Android before API 26 which added
  // pthread_getname_np.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

PosixWaitHandle::~PosixWaitHandle() = default;

thread_local PosixCondition<Thread>* current_thread_condition_ = nullptr;

bool DispatchCurrentThreadUserCallback() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ && current_thread_condition_->DispatchQueuedUserCallbacks();
}

PosixWaiter& CurrentThreadWaiter() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  if (current_thread_condition_) {
    return current_thread_condition_->waiter();
  }
  static thread_local PosixWaiter waiter;
  return waiter;
}

namespace {

constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);

class ScopedAlertableState {
 public:
  explicit ScopedAlertableState(bool alertable) : alertable_(alertable) {
    if (alertable_) {
      alertable_state_ = true;
    }
  }
  ~ScopedAlertableState() {
    if (alertable_) {
      alertable_state_ = false;
    }
  }

 private:
  bool alertable_;
};

std::chrono::steady_clock::time_point ComputeAlertableDeadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max()) {
    return std::chrono::steady_clock::time_point::max();
  }
  return std::chrono::steady_clock::now() + timeout;
}

bool HasAlertableTimeoutElapsed(std::chrono::steady_clock::time_point deadline) {
  return deadline != std::chrono::steady_clock::time_point::max() &&
         std::chrono::steady_clock::now() >= deadline;
}

std::chrono::milliseconds ComputeAlertableWaitTimeout(
    std::chrono::steady_clock::time_point deadline) {
  if (deadline == std::chrono::steady_clock::time_point::max()) {
    return kAlertablePollSlice;
  }
  auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    return std::chrono::milliseconds::zero();
  }
  return std::min(remaining, kAlertablePollSlice);
}

}  // namespace

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  void* native_handle() const override { return handle_.native_handle(); }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner) : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset) : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset, bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread) : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  if (!is_alertable) {
    return posix_wait_handle->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  if (REXCVAR_GET(posix_event_driven_waits)) {
    return PosixConditionBase::WaitEventDriven({&posix_wait_handle->condition()}, false, true,
                                               timeout)
        .first;
  }
  auto deadline = ComputeAlertableDeadline(timeout);

  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    auto result = posix_wait_handle->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto result = WaitResult::kFailed;
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  if (!posix_wait_handle_to_signal->condition().Signal()) {
    return WaitResult::kFailed;
  }

  if (!is_alertable) {
    return posix_wait_handle_to_wait_on->condition().Wait(timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  if (REXCVAR_GET(posix_event_driven_waits)) {
    return PosixConditionBase::WaitEventDriven({&posix_wait_handle_to_wait_on->condition()}, false,
                                               true, timeout)
        .first;
  }
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return WaitResult::kTimeout;
    }
    result = posix_wait_handle_to_wait_on->condition().Wait(ComputeAlertableWaitTimeout(deadline));
    if (result != WaitResult::kTimeout) {
      return result;
    }
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  if (!is_alertable) {
    return PosixConditionBase::WaitMultiple(std::move(conditions), wait_all, timeout);
  }

  ScopedAlertableState alertable_state_guard(true);
  if (REXCVAR_GET(posix_event_driven_waits)) {
    return PosixConditionBase::WaitEventDriven(conditions, wait_all, true, timeout);
  }
  auto deadline = ComputeAlertableDeadline(timeout);
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    if (HasAlertableTimeoutElapsed(deadline)) {
      return std::make_pair(WaitResult::kTimeout, 0);
    }
    auto result = PosixConditionBase::WaitMultiple(std::vector<PosixConditionBase*>(conditions),
                                                   wait_all, ComputeAlertableWaitTimeout(deadline));
    if (result.first != WaitResult::kTimeout) {
      return result;
    }
  }
}

class PosixEvent : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  void Pulse() override {
    using namespace std::chrono_literals;
    handle_.Signal();
    MaybeYield();
    Sleep(10us);
    handle_.Reset();
  }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count), out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count || maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner) : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer : public PosixConditionHandle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(due_time), std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params, std::function<void()> start_routine) {
    auto start_data = new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override { handle_.set_affinity_mask(mask); }

#if !defined(__APPLE__)
  // A hard affinity, unlike Windows' CPU sets: the little cores of a phone
  // run a recorder or a simulation step two to three times slower.
  bool PreferPerformanceCores() override {
#if REX_PLATFORM_LINUX
    // android_thread_cores (linux_thread_cores) placement wins over the generic preference.
    if (PinAndroidNamedThread(handle_.tid(), name())) {
      return true;
    }
#endif
    const uint64_t mask = PerformanceCoreMask();
    if (!mask) {
      return false;
    }
    handle_.set_affinity_mask(mask);
    return true;
  }
#endif

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override { handle_.set_priority(new_priority); }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
#if !REX_PLATFORM_ANDROID
  if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr) != 0) {
    assert_always();
  }
#endif
  set_current_thread_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  current_thread_condition_ = &thread->handle_;
  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID
    thread->handle_.tid_ = static_cast<pid_t>(syscall(SYS_gettid));
#endif
    thread->handle_.state_ = create_suspended ? State::kSuspended : State::kRunning;
    // Arm the suspend counter in the SAME critical section that publishes the
    // started state. Resume() only waits for state_ != kUninitialized, so if it
    // observed the state published here while suspend_count_ was still 0 it
    // would take the "not suspended" path, return false without decrementing,
    // and the wait below would then never be satisfied.
    if (create_suspended) {
      thread->handle_.suspend_count_ = 1;
    }
    thread->handle_.state_signal_.notify_all();
  }

  if (create_suspended) {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_signal_.wait(lock,
                                       [thread] { return thread->handle_.suspend_count_ == 0; });
  }

  start_routine();

  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  {
    std::unique_lock<std::mutex> lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.NotifyAllLocked();
  }

  current_thread_ = nullptr;
  current_thread_condition_ = nullptr;
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine)))
    return nullptr;
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Threads not created by Thread::Create (typically main thread) still need
  // process-wide signal handlers used by suspend/APC machinery.
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  current_thread_condition_ = &current_thread_->condition();
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  // Function must not return
  assert_always();
}

void set_current_thread_name(const std::string_view name) {
#if defined(__APPLE__)
  pthread_setname_np(std::string(name).c_str());
#else
  pthread_setname_np(pthread_self(), std::string(name).c_str());
#endif
#if REX_PLATFORM_ANDROID
  if (!android_pthread_getname_np_ && current_thread_) {
    current_thread_->condition().SetAndroidPreApi26Name(name);
  }
  PinAndroidNamedThread(gettid(), name);
#elif REX_PLATFORM_LINUX
  PinAndroidNamedThread(gettid(), name);
#endif
#if REX_PLATFORM_LINUX
  rex::perf::NoteNamedThread(int64_t(gettid()), name);
#endif
}

static void signal_handler(int signal, siginfo_t* /*info*/, void* /*context*/) {
  switch (GetSystemSignalType(signal)) {
    case SignalType::kThreadSuspend: {
      if (!current_thread_) {
        return;
      }
      current_thread_->WaitSuspended();
    } break;
    case SignalType::kThreadUserCallback: {
      // Callbacks are drained from alertable waits in normal thread context.
      // This signal is only used as a wakeup hint.
    } break;
#if REX_PLATFORM_ANDROID
    case SignalType::kThreadTerminate: {
      pthread_exit(reinterpret_cast<void*>(-1));
    } break;
#endif
    default:
      assert_always();
  }
}

}  // namespace rex::thread
