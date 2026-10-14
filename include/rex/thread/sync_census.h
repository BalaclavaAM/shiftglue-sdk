/**
 * @file        rex/thread/sync_census.h
 * @brief       Probe counters for the global critical region and write watches
 *
 * NN-3.10 / NN-3.12 premise probes, both off by default:
 *
 * - critical_region_census (frames per report, 0 off): every global critical
 *   region acquisition is counted by its source (LockSource), with the
 *   contended ones and the time they blocked, per thread; logged as a
 *   per-frame average every N frames.
 * - write_watch_fault_trace (frames per report, 0 off): write-watch faults
 *   counted apart from announced triggers (guest Protect/Decommit/Release,
 *   file reads, host writes), the fault path's time, the write-watch
 *   protection changes split by who caused them (arm by upload kind,
 *   primitive processor, other arms, restore after a fault or an announced
 *   trigger), and a fault log (physical page, guest lr, thread, upload kind
 *   that last made the page valid) aggregated into writers and recurrent
 *   pages every N frames.
 *
 * When both are off the hot paths cost one relaxed load of a bool.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/runtime_api.h>
#include <rex/thread/mutex.h>

namespace rex::thread::census {

// Called once per frame (rex::perf::ResetFrameCounters): reads the cvars,
// switches the probes and logs a window when it is complete.
void Frame();

// --- Lock census (used by global_critical_region) ---------------------------

// An acquisition by `source`; `waited_ns` is the time a contended one
// blocked (0 when the try-lock succeeded).
void NoteLock(LockSource source, bool contended, int64_t waited_ns);
// A single-reservation section that ran without the mutex (NN-3.1).
void NoteUnlockedReservation();

// --- Write-watch census ------------------------------------------------------

// write_watch_fault_trace is on (mirrored from the cvar every frame).
extern REX_RUNTIME_DATA std::atomic<bool> write_watch_enabled;
inline bool WriteWatchEnabled() { return write_watch_enabled.load(std::memory_order_relaxed); }

// Who a write-watch protection change is for.
enum class ProtectOwner : uint8_t {
  kArmOther,       // an arm with no owner scope (command processor, executor)
  kArmUploadOther, // SharedMemory::MakeRangeValid by upload kind ...
  kArmVertex,
  kArmIndex,
  kArmTexture,
  kArmMemexport,
  kArmGpuWritten,  // SharedMemory::RangeWrittenByGpu (resolves)
  kArmCopyCpu,     // SharedMemory::CopyCpuRange / CopyCpuSnapshot
  kArmPrimitive,   // the primitive processor's converted-index cache
  kRestoreFault,   // TriggerCallbacks restoring protection after a fault
  kRestoreAnnounced,
  kCount
};

// The owner of the arms made on this thread while the scope lives. Does
// nothing (one relaxed load) while the census is off.
class ProtectOwnerScope {
 public:
  explicit ProtectOwnerScope(ProtectOwner owner)
      : active_(WriteWatchEnabled()), previous_(active_ ? Exchange(owner) : owner) {}
  ~ProtectOwnerScope() {
    if (active_) Exchange(previous_);
  }
  ProtectOwnerScope(const ProtectOwnerScope&) = delete;
  ProtectOwnerScope& operator=(const ProtectOwnerScope&) = delete;

 private:
  static ProtectOwner Exchange(ProtectOwner owner);
  bool active_;
  ProtectOwner previous_;
};

// A protection change of `pages` host pages that took `ns`; `restore` for
// TriggerCallbacks, otherwise an arm attributed to the current owner scope.
void NoteProtect(bool restore, int64_t ns, uint32_t pages);

// A write-watch fault on this thread (Memory::AccessViolationCallback,
// around TriggerCallbacks). `guest_lr` is the faulting thread's guest link
// register (0 for a host thread): generated code sets it at every call, so
// it names the function the faulting code was called from.
void BeginFault(uint32_t physical_address, uint32_t guest_lr);
void EndFault(bool handled, int64_t ns);
// From SharedMemory's invalidation callback during a fault on this thread:
// the upload kind that last made the page valid (SharedMemory::UploadKind,
// kPageKindGpuWritten, kPageKindNone) and whether it was valid.
constexpr uint8_t kPageKindGpuWritten = 5;
constexpr uint8_t kPageKindNone = 0xFF;
void NoteFaultPage(uint8_t kind, bool was_valid);
// PhysicalHeap::TriggerCallbacks: `effective` when it found a watch (or
// invalidated an unwatched announced write). Inside a fault scope it counts
// as the fault's trigger, otherwise as an announced one.
void NoteTrigger(bool effective);

}  // namespace rex::thread::census
