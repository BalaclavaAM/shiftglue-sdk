#pragma once

// Probe build (codegen option call_probes): the generated code counts every
// function entry and records the target of every indirect call, and the
// runtime writes the result as JSON at shutdown. The reverse-engineering
// instrument of NEXT_LEVEL_RESEARCH.md NL-2.3: which functions a route runs
// (and which it never runs), and how many distinct targets each bctrl site
// has. Header-only; nothing here runs in a build without the option.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::ppc {

struct ProbeThread {
  std::mutex mutex;
  // (site << 32 | target) -> count
  std::unordered_map<uint64_t, uint64_t> indirect;
};

struct ProbeState {
  std::atomic<std::atomic<uint64_t>*> calls{nullptr};
  uint32_t code_base = 0;
  uint32_t slots = 0;
  std::mutex mutex;
  std::vector<std::shared_ptr<ProbeThread>> threads;
};

inline ProbeState& probeState() {
  static ProbeState state;
  return state;
}

inline ProbeThread& probeThread() {
  thread_local std::shared_ptr<ProbeThread> thread = [] {
    auto t = std::make_shared<ProbeThread>();
    auto& state = probeState();
    std::lock_guard lock(state.mutex);
    state.threads.push_back(t);
    return t;
  }();
  return *thread;
}

// One counter per guest instruction slot, allocated on the first call; the
// generated code passes its image's code range.
inline void ProbeCall(uint64_t code_base, uint64_t code_size, uint32_t site) {
  auto& state = probeState();
  auto* calls = state.calls.load(std::memory_order_acquire);
  if (!calls) {
    std::lock_guard lock(state.mutex);
    calls = state.calls.load(std::memory_order_relaxed);
    if (!calls) {
      state.code_base = static_cast<uint32_t>(code_base);
      state.slots = static_cast<uint32_t>(code_size / 4);
      calls = new std::atomic<uint64_t>[state.slots]();
      state.calls.store(calls, std::memory_order_release);
    }
  }
  uint32_t slot = (site - state.code_base) >> 2;
  if (slot < state.slots)
    calls[slot].fetch_add(1, std::memory_order_relaxed);
}

inline void ProbeIndirect(uint32_t site, uint32_t target) {
  auto& thread = probeThread();
  std::lock_guard lock(thread.mutex);
  ++thread.indirect[(uint64_t(site) << 32) | target];
}

inline bool ProbeActive() { return probeState().calls.load(std::memory_order_acquire) != nullptr; }

// {"calls": [[address, count], ...] (non-zero only), "indirect": [[site, target, count], ...]}
inline bool ProbeDump(const std::filesystem::path& path) {
  auto& state = probeState();
  auto* calls = state.calls.load(std::memory_order_acquire);
  if (!calls)
    return false;
  std::unordered_map<uint64_t, uint64_t> indirect;
  {
    std::lock_guard lock(state.mutex);
    for (const auto& thread : state.threads) {
      std::lock_guard tlock(thread->mutex);
      for (const auto& [key, count] : thread->indirect)
        indirect[key] += count;
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  FILE* f = fopen(path.string().c_str(), "wb");
  if (!f)
    return false;
  fputs("{\"calls\": [", f);
  bool first = true;
  for (uint32_t slot = 0; slot < state.slots; ++slot) {
    uint64_t count = calls[slot].load(std::memory_order_relaxed);
    if (!count)
      continue;
    fprintf(f, "%s[\"%08X\", %llu]", first ? "" : ",", state.code_base + slot * 4,
            static_cast<unsigned long long>(count));
    first = false;
  }
  fputs("], \"indirect\": [", f);
  first = true;
  for (const auto& [key, count] : indirect) {
    fprintf(f, "%s[\"%08X\", \"%08X\", %llu]", first ? "" : ",", uint32_t(key >> 32),
            uint32_t(key), static_cast<unsigned long long>(count));
    first = false;
  }
  fputs("]}\n", f);
  fclose(f);
  return true;
}

}  // namespace rex::ppc
