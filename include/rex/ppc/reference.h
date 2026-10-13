#pragma once

// Lockstep verify support (codegen option reference_functions /
// reference_sample_every, NEXT_LEVEL_RESEARCH.md NL-0.2). The generator emits a
// second copy of selected leaf functions, `ref_sub_X`, with the reference
// options (every register in ctx, no folding, no fast paths) and with its
// stores routed through RefStore*, which log the old and new bytes. A host
// verifier runs the reference copy on a copy of the registers, rolls the
// stores back, runs the optimized function for real, and compares registers
// and the logged bytes. Header-only; a build without references never calls
// this.

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace rex::ppc {

struct RefWrite {
  volatile void* host;
  uint32_t guest;
  uint8_t size;
  uint8_t old[8];
  uint8_t neu[8];
};

inline std::vector<RefWrite>& RefLog() {
  thread_local std::vector<RefWrite> log;
  return log;
}

template <typename T>
inline void RefStore(volatile T* host, uint32_t guest, T value) {
  RefWrite w{};
  w.host = host;
  w.guest = guest;
  w.size = sizeof(T);
  T old = *host;
  std::memcpy(w.old, &old, sizeof(T));
  std::memcpy(w.neu, &value, sizeof(T));
  RefLog().push_back(w);
  *host = value;
}

inline void RefLogBegin() { RefLog().clear(); }

// Undo the logged stores, newest first.
inline void RefRollback() {
  auto& log = RefLog();
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    std::memcpy(const_cast<void*>(it->host), it->old, it->size);
  }
}

// After the optimized function ran for real: the first logged store whose
// final bytes differ from what the reference left there, or nullptr. Only the
// last store to each (address, size) counts: a function may write a word
// several times, and only its final value is observable.
inline const RefWrite* RefFirstDifference() {
  auto& log = RefLog();
  std::vector<std::pair<volatile void*, uint8_t>> seen;
  seen.reserve(log.size());
  for (auto it = log.rbegin(); it != log.rend(); ++it) {
    bool later = false;
    for (const auto& s : seen) {
      if (s.first == it->host && s.second == it->size) {
        later = true;
        break;
      }
    }
    if (later)
      continue;
    seen.emplace_back(it->host, it->size);
    if (std::memcmp(const_cast<const void*>(it->host), it->neu, it->size) != 0)
      return &*it;
  }
  return nullptr;
}

}  // namespace rex::ppc
