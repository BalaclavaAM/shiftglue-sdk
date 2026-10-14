/**
 * @file        audio/host_frame_source.h
 * @brief       Host-built render-driver frames in place of the guest callback
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

namespace rex::audio {

/// One render-driver frame: 256 samples per channel, six channels, planar
/// (all of channel 0, then channel 1, ...), each sample a big-endian float as
/// the guest submits it through XAudioSubmitRenderDriverFrame.
constexpr uint32_t kRenderFrameChannels = 6;
constexpr uint32_t kRenderFrameChannelSamples = 256;
constexpr uint32_t kRenderFrameWords = kRenderFrameChannels * kRenderFrameChannelSamples;

/**
 * Lets a title build a render-driver client's frames on the host. The Audio
 * Worker asks the source before every pump of the client: when BuildFrame
 * fills the frame, the worker submits it straight to the driver and does not
 * run the guest callback for that block; otherwise it runs the callback as
 * usual, bracketed by GuestBlockBegin and GuestBlockEnd. Every frame the guest
 * submits for the client is also shown to ObserveGuestFrame, so a source can
 * compare its own block with the guest's (a shadow mode).
 *
 * BuildFrame, GuestBlockBegin and GuestBlockEnd run on the Audio Worker.
 * ObserveGuestFrame runs on the thread that calls XAudioSubmitRenderDriverFrame
 * (the worker, inside the guest callback, for a title that submits from its
 * callback) with the audio system's client lock held, so it must not block.
 */
class HostFrameSource {
 public:
  virtual ~HostFrameSource() = default;

  /// Whether the source drives the client being registered with this guest
  /// callback. Called once per registration, with the client lock held.
  virtual bool Accepts(uint32_t callback, uint32_t callback_arg) = 0;

  /// Fills `frame` (kRenderFrameWords big-endian words) and returns true to
  /// submit it in place of running the guest callback for this block.
  virtual bool BuildFrame(uint32_t* frame) = 0;

  virtual void GuestBlockBegin() {}
  virtual void GuestBlockEnd() {}

  /// A frame the guest submitted for the client (kRenderFrameWords words in
  /// guest memory, big-endian).
  virtual void ObserveGuestFrame(const uint32_t* frame) { (void)frame; }
};

/// Sets the process-wide source offered to every render-driver client
/// registered afterwards (null clears it for later registrations). The
/// source must outlive the audio system. Safe to call from any thread, and
/// meant to be called before the title starts.
void SetHostFrameSource(HostFrameSource* source);
HostFrameSource* GetHostFrameSource();

}  // namespace rex::audio
