#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rex::graphics {

enum class Fh1InspectionView : uint32_t { kColor, kWireframe, kOverdraw };

// CPU-owned, immutable after publication. No renderer image, view or descriptor
// may escape into this contract. UI uploads belong to the UI drawer's lifetime.
struct Fh1InspectionImage {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  std::string error;
};
struct Fh1InspectionSurface {
  uint32_t key = 0, base = 0, pitch = 0, format = 0;
  uint32_t width = 0, height = 0, samples = 0, draws = 0;
  bool depth = false;
  std::string format_name;
  Fh1InspectionImage image;
};
struct Fh1InspectionResolve {
  uint32_t address = 0, length = 0, source_key = 0;
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  // The source rectangle before destination packing, endian and sample selection.
  Fh1InspectionImage image;
};
struct Fh1InspectionTexture {
  uint32_t address = 0, width = 0, height = 0, format = 0;
  uint64_t hash = 0;
  bool hash_valid = false;
};
struct Fh1FrameInspection {
  uint64_t frame = 0;
  Fh1InspectionView view = Fh1InspectionView::kColor;
  std::vector<Fh1InspectionSurface> surfaces;
  std::vector<Fh1InspectionResolve> resolves;
  std::vector<Fh1InspectionTexture> textures;
  bool truncated = false;
};

// One mailbox per command processor. Requests are consumed at a frame boundary;
// the disabled path does no allocation, enumeration, hashing or GPU work.
class Fh1FrameInspector {
 public:
  struct Capabilities {
    bool wireframe = false, overdraw = false, available = false;
  };
  void SetCapabilities(Capabilities caps) {
    std::lock_guard lock(mutex_);
    caps_ = caps;
  }
  Capabilities capabilities() const {
    std::lock_guard lock(mutex_);
    return caps_;
  }
  bool Request(Fh1InspectionView view) {
    std::lock_guard lock(mutex_);
    if (!caps_.available || busy_ || uint32_t(view) > uint32_t(Fh1InspectionView::kOverdraw) ||
        (view == Fh1InspectionView::kWireframe && !caps_.wireframe) ||
        (view == Fh1InspectionView::kOverdraw && !caps_.overdraw))
      return false;
    view_ = view;
    busy_ = true;
    requested_.store(true, std::memory_order_release);
    return true;
  }
  bool Begin(Fh1InspectionView& view, uint64_t& generation) {
    if (!requested_.load(std::memory_order_acquire))
      return false;
    std::lock_guard lock(mutex_);
    if (!requested_.exchange(false))
      return false;
    view = view_;
    generation = generation_;
    return true;
  }
  void Publish(std::shared_ptr<const Fh1FrameInspection> frame, uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (generation != generation_)
      return;
    frame_ = std::move(frame);
    busy_ = false;
  }
  std::shared_ptr<const Fh1FrameInspection> snapshot() const {
    std::lock_guard lock(mutex_);
    return frame_;
  }
  bool busy() const {
    std::lock_guard lock(mutex_);
    return busy_;
  }
  void SetStatus(std::string status) {
    std::lock_guard lock(mutex_);
    status_ = std::move(status);
  }
  std::string status() const {
    std::lock_guard lock(mutex_);
    return status_;
  }
  void Clear() {
    std::lock_guard lock(mutex_);
    ++generation_;
    requested_.store(false, std::memory_order_release);
    frame_.reset();
    busy_ = false;
    status_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::atomic<bool> requested_{false};
  Capabilities caps_;
  Fh1InspectionView view_ = Fh1InspectionView::kColor;
  uint64_t generation_ = 0;
  bool busy_ = false;
  std::shared_ptr<const Fh1FrameInspection> frame_;
  std::string status_;
};
}  // namespace rex::graphics
