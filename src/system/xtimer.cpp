/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/chrono/chrono.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/system/xthread.h>
#include <rex/system/xtimer.h>
#include <rex/session_probe.h>
#include <rex/cs_scheduler.h>

namespace rex::system {

XTimer::XTimer(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
#if defined(REXGLUE_ENABLE_CS_PROBES)
  if (verification_event_) Cancel();
#endif
}

void XTimer::Initialize(uint32_t timer_type) {
#if defined(REXGLUE_ENABLE_CS_PROBES)
  if (cs_scheduler::Enabled()) {
    assert_true(timer_type <= 1);
    verification_event_ = timer_type ? rex::thread::Event::CreateAutoResetEvent(false) :
                                      rex::thread::Event::CreateManualResetEvent(false);
    return;
  }
#endif
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = rex::thread::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = rex::thread::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                          uint32_t routine_arg, bool resume) {
  using rex::chrono::WinSystemClock;
  using rex::chrono::XSystemClock;
  // Caller is checking for STATUS_TIMER_RESUME_IGNORED.
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }
#if defined(REXGLUE_ENABLE_CS_PROBES)
  if (verification_event_) {
    Cancel(); verification_event_->Reset();
    verification_stop_=false; verification_done_=false;
    const auto caller=XThread::GetCurrentThread();
    const uint64_t deadline=due_time<0 ? cs_scheduler::Now()+uint64_t(-due_time)*100 :
        (uint64_t(due_time)>cs_scheduler::Epoch() ? (uint64_t(due_time)-cs_scheduler::Epoch())*100 : cs_scheduler::Now());
    const uint64_t tag=UINT64_C(0x8000000100000000)|handle();
    verification_timer_=std::thread([this,caller,deadline,tag,period_ms,routine,routine_arg] {
      cs_scheduler::Actor scheduled(tag);
      uint64_t next=deadline;
      while (!verification_stop_) {
        if (cs_scheduler::Now()<next) {
          cs_scheduler::Point(0,cs_scheduler::PointKind::kWait,true,next-cs_scheduler::Now());
          continue;
        }
        verification_event_->Set();
        if (routine) {
          const auto time=cs_scheduler::Epoch()+cs_scheduler::Now()/100;
          caller->EnqueueApc(routine,routine_arg,uint32_t(time),uint32_t(time>>32));
        }
        if (!period_ms) break;
        next+=uint64_t(period_ms)*1000000;
      }
      verification_done_=true;
    });
    return X_STATUS_SUCCESS;
  }
#endif

  period_ms = chrono::Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = rex::chrono::hundrednanoseconds(-due_time);
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = std::chrono::clock_cast<WinSystemClock>(XSystemClock::from_file_time(due_time));
  }

  // Stash routine for callback.
  callback_thread_ = XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  std::function<void()> callback = nullptr;
  if (callback_routine_ || session_probe::Enabled()) {
    callback = [this]() {
      if (session_probe::Enabled()) {
        session_probe::SetHostRole(UINT64_C(0x100000000) | handle());
        session_probe::Delivery(session_probe::Kind::kTimerFire, handle(),
            (uint64_t(callback_routine_) << 32) | callback_routine_arg_);
      }
      if (!callback_routine_) return;
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = rex::chrono::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      REXSYS_DEBUG("XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
                   callback_routine_, callback_routine_arg_, time_low, time_high);
      callback_thread_->EnqueueApc(callback_routine_, callback_routine_arg_, time_low, time_high);
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result =
        timer_->SetRepeatingAt(due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
#if defined(REXGLUE_ENABLE_CS_PROBES)
  if (verification_event_) {
    verification_stop_=true;
    cs_scheduler::Wake(UINT64_C(0x8000000100000000)|handle());
    while (!verification_done_) {
      if (cs_scheduler::InActor()) cs_scheduler::Point(0,cs_scheduler::PointKind::kWait,true);
      else std::this_thread::yield();
    }
    if (verification_timer_.joinable()) verification_timer_.join();
    return X_STATUS_SUCCESS;
  }
#endif
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace rex::system
