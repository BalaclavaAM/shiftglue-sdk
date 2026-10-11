/**
 * @file        tests/unit/core/threading_wait_test.cpp
 * @brief       Semantics of multi-object and alertable waits, which POSIX
 *              hosts implement with per-thread waiters instead of polling.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/thread.h>

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Semaphore;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;

TEST_CASE("WaitAny returns the index of the object signalled later", "[threading]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto b = Event::CreateAutoResetEvent(false);
  std::thread signaller([&] {
    std::this_thread::sleep_for(20ms);
    b->Set();
  });
  auto result = rex::thread::WaitAny({a.get(), b.get()}, false, 5000ms);
  signaller.join();
  CHECK(result.first == WaitResult::kSuccess);
  CHECK(result.second == 1);
  // The auto-reset event was consumed by the wait.
  CHECK(rex::thread::WaitAny({a.get(), b.get()}, false, 0ms).first == WaitResult::kTimeout);
}

TEST_CASE("WaitAny times out when nothing is signalled", "[threading]") {
  auto a = Event::CreateManualResetEvent(false);
  auto b = Event::CreateManualResetEvent(false);
  auto start = std::chrono::steady_clock::now();
  auto result = rex::thread::WaitAny({a.get(), b.get()}, false, 30ms);
  CHECK(result.first == WaitResult::kTimeout);
  CHECK(std::chrono::steady_clock::now() - start >= 25ms);
}

TEST_CASE("WaitAll waits for every object and consumes each once", "[threading]") {
  auto a = Event::CreateAutoResetEvent(false);
  auto sem = Semaphore::Create(0, 4);
  std::thread signaller([&] {
    a->Set();
    std::this_thread::sleep_for(20ms);
    sem->Release(2, nullptr);
  });
  CHECK(rex::thread::WaitAll({a.get(), sem.get()}, false, 5000ms) == WaitResult::kSuccess);
  signaller.join();
  // One semaphore count is left; the auto-reset event is clear.
  CHECK(rex::thread::Wait(sem.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(rex::thread::Wait(sem.get(), false, 0ms) == WaitResult::kTimeout);
  CHECK(rex::thread::Wait(a.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("Many signals wake many multi-object waiters", "[threading]") {
  constexpr int kWaiters = 4;
  constexpr int kRounds = 200;
  auto stop = Event::CreateManualResetEvent(false);
  auto work = Semaphore::Create(0, kWaiters * kRounds);
  std::atomic<int> taken{0};
  std::vector<std::thread> waiters;
  for (int i = 0; i < kWaiters; ++i) {
    waiters.emplace_back([&] {
      while (true) {
        auto result = rex::thread::WaitAny({stop.get(), work.get()}, false, 5000ms);
        if (result.first != WaitResult::kSuccess || result.second == 0) {
          return;
        }
        taken.fetch_add(1);
      }
    });
  }
  for (int i = 0; i < kRounds * kWaiters; ++i) {
    work->Release(1, nullptr);
    if (i % 16 == 0) {
      std::this_thread::yield();
    }
  }
  auto deadline = std::chrono::steady_clock::now() + 5s;
  while (taken.load() < kRounds * kWaiters && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  stop->Set();
  for (auto& waiter : waiters) {
    waiter.join();
  }
  CHECK(taken.load() == kRounds * kWaiters);
}

TEST_CASE("A queued user callback ends an alertable wait", "[threading]") {
  auto never = Event::CreateManualResetEvent(false);
  auto other = Event::CreateManualResetEvent(false);
  auto ready = Event::CreateManualResetEvent(false);
  std::atomic<int> ran{0};
  std::atomic<int> single{-1};
  std::atomic<int> multiple{-1};
  rex::thread::Thread::CreationParameters params;
  auto thread = rex::thread::Thread::Create(params, [&] {
    ready->Set();
    single = static_cast<int>(rex::thread::Wait(never.get(), true, 10000ms));
    ready->Set();
    multiple =
        static_cast<int>(rex::thread::WaitAny({never.get(), other.get()}, true, 10000ms).first);
  });
  REQUIRE(thread);
  REQUIRE(rex::thread::Wait(ready.get(), false, 5000ms) == WaitResult::kSuccess);
  ready->Reset();
  std::this_thread::sleep_for(20ms);
  thread->QueueUserCallback([&] { ran.fetch_add(1); });
  REQUIRE(rex::thread::Wait(ready.get(), false, 5000ms) == WaitResult::kSuccess);
  std::this_thread::sleep_for(20ms);
  thread->QueueUserCallback([&] { ran.fetch_add(1); });
  REQUIRE(rex::thread::Wait(thread.get(), false, 5000ms) == WaitResult::kSuccess);
  CHECK(single.load() == static_cast<int>(WaitResult::kUserCallback));
  CHECK(multiple.load() == static_cast<int>(WaitResult::kUserCallback));
  CHECK(ran.load() == 2);
}
