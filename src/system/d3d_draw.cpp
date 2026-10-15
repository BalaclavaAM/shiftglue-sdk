#include <rex/system/d3d_draw.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace rex::system::d3d_draw {
namespace {

// The guest runs at most a frame or two ahead of the decoder; menus make
// about 220 qualifying draws a frame.
constexpr uint32_t kSlotCount = 1024;

struct Slot {
  // 0 while being written; the id once complete.
  std::atomic<uint32_t> id{0};
  Description description;
};

std::unique_ptr<Slot[]> g_slots;
std::once_flag g_slots_once;
std::atomic<uint32_t> g_next_id{0};
std::atomic<uint32_t> g_published{0};

Slot* Slots() {
  std::call_once(g_slots_once, [] { g_slots.reset(new Slot[kSlotCount]); });
  return g_slots.get();
}

constexpr size_t kHeaderBytes = offsetof(Description, words);

}  // namespace

uint32_t NextId() {
  uint32_t id = g_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!id) id = g_next_id.fetch_add(1, std::memory_order_relaxed) + 1;
  return id;
}

void Publish(const Description& description) {
  Slot& slot = Slots()[description.id % kSlotCount];
  slot.id.store(0, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  std::memcpy(&slot.description, &description,
              kHeaderBytes + size_t(description.word_count) * sizeof(uint32_t));
  slot.id.store(description.id, std::memory_order_release);
  uint32_t published = g_published.load(std::memory_order_relaxed);
  while (int32_t(description.id - published) > 0 &&
         !g_published.compare_exchange_weak(published, description.id,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
  }
}

bool Fetch(uint32_t id, Description* out) {
  if (!id) return false;
  Slot& slot = Slots()[id % kSlotCount];
  // A marker can reach the decoder before its call returns when the device
  // kicks a segment inside the call: wait briefly for the publish.
  if (int32_t(id - g_published.load(std::memory_order_acquire)) > 0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    while (int32_t(id - g_published.load(std::memory_order_acquire)) > 0 &&
           slot.id.load(std::memory_order_acquire) != id) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::yield();
    }
  }
  if (slot.id.load(std::memory_order_acquire) != id) return false;
  std::memcpy(out, &slot.description, kHeaderBytes);
  if (out->word_count > kMaxWords) return false;
  std::memcpy(out->words, slot.description.words, size_t(out->word_count) * sizeof(uint32_t));
  std::atomic_thread_fence(std::memory_order_acquire);
  return slot.id.load(std::memory_order_relaxed) == id && out->id == id;
}

}  // namespace rex::system::d3d_draw
