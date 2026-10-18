// CS-0.1 per-thread boundary log. Diagnostics only; mode 0 is the default.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rex::session_probe {
enum class Kind : uint32_t {
  kTimebase = 1, kSystemTime, kUptime, kInputResult, kInputState,
  kSyncRequest, kSyncResult, kIoRead, kIoWrite, kTimerFire, kPm4Frame,
  kThreadStart, kSignal, kIoData, kGpuInterrupt, kGpuFence,
  kPm4Words,
  kPerformanceFrequency, kVblankDelivery, kTimestampDelivery, kApcDelivery,
  kHostEpoch,
  kInputCapsResult, kInputCapsData, kInputKeyResult, kInputKeyData,
  kIoReadRequest, kIoReadData, kSemaphoreRelease,
};
bool Enabled();
bool Replaying();
void SetThreadTag(uint64_t tag, uint32_t entry = 0);
void SetHostRole(uint64_t role);
uint64_t CurrentTag();
uint64_t Site(const void* return_address);
void Delivery(Kind kind, uint64_t key, uint64_t value);
std::optional<uint32_t> PeekWaitResult(uint64_t key);
uint64_t Scalar(Kind kind, uint64_t key, uint64_t value, bool replay_value = false);
void Blob(Kind kind, uint64_t key, void* data, uint32_t size);
void Flush();
void Bytes(Kind kind, uint64_t key, void* data, uint32_t size);
void Require(bool condition, const char* reason);
struct Suppress {
  Suppress();
  ~Suppress();
};
struct SyncBoundary {
  uint64_t key;
  bool enabled;
  std::optional<uint32_t> expected;
  SyncBoundary(uint64_t key, uint64_t timeout) : key(key), enabled(Enabled()) {
    if (enabled) {
      Scalar(Kind::kSyncRequest, key, timeout);
      expected = PeekWaitResult(key);
    }
  }
  uint32_t Finish(uint32_t result) {
    if (enabled) Scalar(Kind::kSyncResult, key, result);
    return expected.value_or(result);
  }
};
inline uint64_t Digest(const void* data, size_t size,
                       uint64_t hash = UINT64_C(14695981039346656037)) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ p[i]) * UINT64_C(1099511628211);
  return hash;
}
}  // namespace rex::session_probe
