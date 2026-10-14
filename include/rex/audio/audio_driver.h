/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/kernel.h>
#include <rex/memory.h>

namespace rex::audio {

class AudioDriver {
 public:
  explicit AudioDriver(memory::Memory* memory);
  virtual ~AudioDriver();

  virtual void SubmitFrame(uint32_t samples_ptr) = 0;
  // A frame built on the host (rex/audio/host_frame_source.h): the same
  // layout as a guest frame, kRenderFrameWords big-endian words.
  virtual void SubmitHostFrame(const uint32_t* frame) = 0;

 protected:
  inline uint8_t* TranslatePhysical(uint32_t guest_address) const {
    return memory_->TranslatePhysical(guest_address);
  }

  memory::Memory* memory_ = nullptr;
};

}  // namespace rex::audio
