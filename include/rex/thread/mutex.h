/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include <rex/runtime_api.h>

namespace rex::thread {

// Who takes the global critical region, for critical_region_census (NN-3.10,
// rex/thread/sync_census.h). Untagged acquisitions count as kOther.
enum class LockSource : uint8_t {
  kOther,
  kGuestSection,      // recompiled interrupts-off sections (mtmsrd r13)
  kGuestReservation,  // single-reservation sections that took the mutex
  kObjectLookup,      // ObjectTable::LookupObject (handle imports)
  kNativeObject,      // XObject::GetNativeObject (pointer imports)
  kWriteWatchArm,     // PhysicalHeap::EnableAccessCallbacks
  kWriteWatchFault,   // the access-violation handler
  kWriteWatchAnnounced,  // guest Protect/Decommit/Release, host-announced writes
  kSharedMemory,      // GPU shared memory validity and watches
  kTextureCache,
  kInterrupt,         // FunctionDispatcher::ExecuteInterrupt
  kNotify,            // XNotifyListener
  kCount
};

// The global critical region mutex singleton.
// This must guard any operation that may suspend threads or be sensitive to
// being suspended such as global table locks and such.
// To prevent deadlocks this should be the first lock acquired and be held
// for the entire duration of the critical region (longer than any other lock).
//
// As a general rule if some code can only be accessed from the guest you can
// guard it with only the global critical region and be assured nothing else
// will touch it. If it will be accessed from non-guest threads you may need
// some additional protection.
//
// You can think of this as disabling interrupts in the guest. The thread in the
// global critical region has exclusive access to the entire system and cannot
// be preempted. This also means that all activity done while in the critical
// region must be extremely fast (no IO!), as it has the chance to block any
// other thread until its done.
//
// For example, in the following situation thread 1 will not be able to suspend
// thread 0 until it has exited its critical region, preventing it from being
// suspended while holding the table lock:
//   [thread 0]:
//     DoKernelStuff():
//       auto global_lock = global_critical_region_.Acquire();
//       std::lock_guard<std::mutex> table_lock(table_mutex_);
//       table_->InsertStuff();
//   [thread 1]:
//     MySuspendThread():
//       auto global_lock = global_critical_region_.Acquire();
//       ::SuspendThread(thread0);
//
// To use the region it's strongly recommended that you keep an instance near
// the data requiring it. This makes it clear to those reading that the data
// is protected by the global critical region. For example:
// class MyType {
//   // Implies my_list_ is protected:
//   xe::global_critical_region global_critical_region_;
//   std::list<...> my_list_;
// };
class global_critical_region {
 public:
  static std::recursive_mutex& mutex();

  // Locks the global mutex, counting the acquisitions that had to wait
  // (the kCriticalRegionContentions performance counter). With
  // critical_region_census on, every acquisition is also counted by source.
  static void LockCounted(LockSource source = LockSource::kOther) {
    if (census_enabled_.load(std::memory_order_relaxed)) {
      LockCensus(source);
      return;
    }
    if (!mutex().try_lock()) {
      LockContended();
    }
  }

  // Recompiled guest code: sections with interrupts disabled (mtmsrd r13 to
  // enter, any other register to leave) hold the region, and mfmsr reads
  // whether this thread is in one (0, or 0x8000 with interrupts enabled).
  static void GuestEnter() { GuestEnterFrom(LockSource::kGuestSection); }
  // NN-3.1: a section the code generator found to hold one load-reserve/
  // store-conditional pair and nothing else. Its store-conditional is a host
  // compare-exchange, so with guest_reservation_sections_unlocked it runs
  // without the mutex; mfmsr still sees interrupts off. The section's leave
  // is the next GuestLeave this thread makes, since the section holds no
  // call or nested section.
  static void GuestEnterReservation() {
    if (guest_hold_log_ms_ < 0) {
      GuestHoldLogInit();  // also reads the cvar
    }
    if (!reservation_sections_unlocked_.load(std::memory_order_relaxed)) {
      GuestEnterFrom(LockSource::kGuestReservation);
      return;
    }
    ++unlocked_depth_;
    ++guest_depth_;
    CountReservationSection();
  }
  static void GuestLeave() {
    if (unlocked_depth_) {
      --unlocked_depth_;
      --guest_depth_;
      return;
    }
    if (--guest_depth_ == 0 && guest_hold_log_ms_ > 0) {
      GuestHoldEnd();
    }
    mutex().unlock();
  }
  static uint64_t GuestCheck() { return guest_depth_ ? 0 : 0x8000; }

  // Acquires a lock on the global critical section.
  // Use this when keeping an instance is not possible. Otherwise, prefer
  // to keep an instance of global_critical_region near the members requiring
  // it to keep things readable.
  static std::unique_lock<std::recursive_mutex> AcquireDirect(
      LockSource source = LockSource::kOther) {
    LockCounted(source);
    return std::unique_lock<std::recursive_mutex>(mutex(), std::adopt_lock);
  }

  // Acquires a lock on the global critical section.
  inline std::unique_lock<std::recursive_mutex> Acquire(LockSource source = LockSource::kOther) {
    return AcquireDirect(source);
  }

  // Acquires a deferred lock on the global critical section.
  inline std::unique_lock<std::recursive_mutex> AcquireDeferred() {
    return std::unique_lock<std::recursive_mutex>(mutex(), std::defer_lock);
  }

  // Tries to acquire a lock on the glboal critical section.
  // Check owns_lock() to see if the lock was successfully acquired.
  inline std::unique_lock<std::recursive_mutex> TryAcquire() {
    return std::unique_lock<std::recursive_mutex>(mutex(), std::try_to_lock);
  }

 private:
  // Counts the contention and the time blocked (kCriticalRegionBlockedNs).
  static void LockContended();
  // LockCounted with critical_region_census on: the same, plus the census.
  static void LockCensus(LockSource source);
  static void GuestEnterFrom(LockSource source) {
    LockCounted(source);
    if (guest_depth_++ == 0 && GuestHoldLogEnabled()) {
      GuestHoldBegin();
    }
  }
  static inline thread_local int32_t guest_depth_ = 0;

  // critical_region_hold_log_ms: guest sections held at least that long are
  // logged with the recompiled function that entered them.
  static bool GuestHoldLogEnabled() {
    if (guest_hold_log_ms_ < 0) {
      GuestHoldLogInit();
    }
    return guest_hold_log_ms_ > 0;
  }
  static void GuestHoldLogInit();
  static void GuestHoldBegin();
  static void GuestHoldEnd();
  // Defined once in mutex.cpp, not inline: an inline static is a weak symbol
  // that each shared library resolves to its own copy on Android, so the
  // generated code in libmain never saw the runtime's writes. The
  // thread_locals below stay inline and are only used by the generated code,
  // except the hold timestamps, which GuestHoldBegin/End keep in the runtime.
  // On Windows the generated code reads this and
  // reservation_sections_unlocked_ through rexruntime's import table
  // (REX_RUNTIME_DATA).
  static REX_RUNTIME_DATA int32_t guest_hold_log_ms_;
  static void CountReservationSection();

 public:
  // guest_reservation_sections_unlocked (NN-3.1), mirrored for generated code.
  static REX_RUNTIME_DATA std::atomic<bool> reservation_sections_unlocked_;
  // critical_region_census is on (set by census::Frame every frame).
  static REX_RUNTIME_DATA std::atomic<bool> census_enabled_;

 private:
  static inline thread_local int32_t unlocked_depth_ = 0;
  static inline thread_local int64_t guest_hold_start_ns_ = 0;
  static inline thread_local const void* guest_hold_site_ = nullptr;
};

}  // namespace rex::thread
