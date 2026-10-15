#pragma once

// D3D-level draws (Pinyon Shift docs/D3D_RENDERER_RESEARCH.md DL-1.2): a
// draw described from the guest D3D device's state at the draw call, handed
// from the title's hooks (game side) to the command processor, which draws
// it in place of the packets the device wrote for it.
//
// Ordering: at the draw call the game side writes a marker (a PM4 NOP,
// kMarkerMagic, the description id and the mode) into the device's command
// buffer before the call's own packets, and publishes the description here
// when the call returns. The decoder meets the marker where the call's
// packets start, so the draw lands in the same place in the host command
// stream; it then reads the description by id. A description it cannot read
// (overwritten, declined, too large) leaves the packets to the decoder.
//
// Description format: a header and a word stream of entries, each a kind
// word ((kind << 16) | count) and a first-register word, then:
// - kEntryRegisters: count register values, big-endian (as the device
//   shadow holds them);
// - kEntryRegistersFromMemory: one guest physical address of count
//   big-endian values (a shader's literal constants);
// - kEntryShader: the shader type in the first-register word; one guest
//   physical address and, if count > 1, count - 1 microcode words copied
//   from the command buffer (a load the device wrote inline).
// Entries apply in order. Lives in rexruntime: the game module and the GPU
// plugin both link it.

#include <atomic>
#include <cstdint>

namespace rex::system::d3d_draw {

// "D3DL", the first word of a marker NOP's payload.
constexpr uint32_t kMarkerMagic = 0x4433444C;
// Type-3 NOP, three payload words: magic, id, mode.
constexpr uint32_t kMarkerHeader = 0xC0021000;

enum Mode : uint32_t {
  kModeOff = 0,
  // The decoder runs the packets and compares them with the description.
  kModeVerify = 1,
  // The decoder skips the packets the description covers and draws it.
  kModeDraw = 2,
};

enum EntryKind : uint32_t {
  kEntryRegisters = 1,
  kEntryRegistersFromMemory = 2,
  kEntryShader = 3,
};

enum DescriptionFlags : uint32_t {
  // The draw call did not qualify after its prologue ran (target, scope):
  // the decoder leaves the packets alone.
  kFlagDeclined = 1,
  // The description did not fit: the same.
  kFlagOverflow = 2,
};

constexpr uint32_t kMaxWords = 3072;

struct Description {
  uint32_t id = 0;
  uint32_t flags = 0;
  // The draw packet the call writes: PM4_DRAW_INDX's viz query word,
  // VGT_DRAW_INITIATOR, VGT_DMA_BASE and VGT_DMA_SIZE.
  uint32_t draw[4] = {};
  uint32_t word_count = 0;
  uint32_t words[kMaxWords];
};

// Publishes a finished description (copies description.word_count words).
void Publish(const Description& description);

// Copies the description with this id into *out if it is still held and
// complete; false if it was overwritten or never published.
bool Fetch(uint32_t id, Description* out);

// A new id (never 0).
uint32_t NextId();

}  // namespace rex::system::d3d_draw
