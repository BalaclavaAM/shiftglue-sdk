/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/stream.h>
#include <rex/types.h>

namespace rex::stream {

BitStream::BitStream(uint8_t* buffer, size_t size_in_bits)
    : buffer_(buffer), size_bits_(size_in_bits) {}

BitStream::~BitStream() = default;

void BitStream::SetOffset(size_t offset_bits) {
  assert_false(offset_bits > size_bits_);
  offset_bits_ = std::min(offset_bits, size_bits_);
}

size_t BitStream::BitsRemaining() {
  return size_bits_ - offset_bits_;
}

uint64_t BitStream::Peek(size_t num_bits) {
  // FYI: The reason we can't copy more than 57 bits is:
  // 57 = 7 * 8 + 1 - that can only span a maximum of 8 bytes.
  // We can't read in 9 bytes (easily), so we limit it.
  assert_false(num_bits > 57);
  assert_false(offset_bits_ + num_bits > size_bits_);

  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  // offset -->
  // ..[junk]..| target bits |....[junk].............
  uint64_t bits = *(uint64_t*)(buffer_ + offset_bytes);

  // We need the data in little endian.
  // TODO: Have a flag specifying endianness of data?
  bits = rex::byte_swap(bits);

  // Shift right
  // .....[junk]........| target bits |
  bits >>= 64 - (rel_offset_bits + num_bits);

  // AND with mask
  // ...................| target bits |
  bits &= (1ULL << num_bits) - 1;

  return bits;
}

uint64_t BitStream::Read(size_t num_bits) {
  uint64_t val = Peek(num_bits);
  Advance(num_bits);

  return val;
}

// TODO: This is totally not tested!
bool BitStream::Write(uint64_t val, size_t num_bits) {
  assert_false(num_bits > 57);
  assert_false(offset_bits_ + num_bits >= size_bits_);

  size_t offset_bytes = offset_bits_ >> 3;
  size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);

  // Construct a mask
  uint64_t mask = (1ULL << num_bits) - 1;
  mask <<= 64 - (rel_offset_bits + num_bits);
  mask = ~mask;

  // Shift the value left into position.
  val <<= 64 - (rel_offset_bits + num_bits);

  // offset ----->
  // ....[junk]...| target bits w/ junk |....[junk]......
  uint64_t bits = *(uint64_t*)(buffer_ + offset_bytes);

  // AND with mask
  // ....[junk]...| target bits (0) |........[junk]......
  bits &= mask;

  // OR with val
  // ....[junk]...| target bits (val) |......[junk]......
  bits |= val;

  // Store into the bitstream.
  *(uint64_t*)(buffer_ + offset_bytes) = bits;

  // Advance the bitstream forward.
  Advance(num_bits);

  return true;
}

namespace {

// Reads up to 8 bits starting at bit position `pos` without touching bytes
// past the one(s) that hold them. The caller guarantees pos + n <= size.
uint8_t ReadSmall(const uint8_t* buffer, size_t pos, size_t n) {
  const size_t byte = pos >> 3;
  const size_t rel = pos & 7;
  uint32_t window = uint32_t(buffer[byte]) << 8;
  if (rel + n > 8) {
    window |= buffer[byte + 1];
  }
  return uint8_t((window >> (16 - rel - n)) & ((1u << n) - 1));
}

// ORs the low `n` bits of `bits` into dest so that they occupy MSB-first bit
// positions [first, first + n) of the byte, leaving the other bits alone.
void WriteSmall(uint8_t* dest, size_t first, size_t n, uint8_t bits) {
  const size_t shift = 8 - first - n;
  const uint8_t mask = uint8_t(((1u << n) - 1) << shift);
  *dest = uint8_t((*dest & ~mask) | ((uint32_t(bits) << shift) & mask));
}

}  // namespace

size_t BitStream::Copy(uint8_t* dest_buffer, size_t num_bits) {
  const size_t rel_offset_bits = offset_bits_ & 7;

  // Never read past the end of the stream, whatever the caller asks for.
  size_t bits_left = std::min(num_bits, size_bits_ - offset_bits_);
  size_t out_offset_bytes = 0;

  // First: Copy the first few bits up to a byte boundary. The output keeps
  // the source's bit alignment, so this may be fewer than 8 - rel bits.
  if (rel_offset_bits && bits_left) {
    const size_t head_bits = std::min(bits_left, size_t(8) - rel_offset_bits);
    WriteSmall(dest_buffer, rel_offset_bits, head_bits,
               ReadSmall(buffer_, offset_bits_, head_bits));
    bits_left -= head_bits;
    Advance(head_bits);
    out_offset_bytes++;
  }

  // Second: Use memcpy for the whole bytes left (offset is byte aligned now,
  // unless the head consumed everything).
  if (bits_left >= 8) {
    const size_t whole_bytes = bits_left / 8;
    std::memcpy(dest_buffer + out_offset_bytes, buffer_ + (offset_bits_ >> 3), whole_bytes);
    out_offset_bytes += whole_bytes;
    Advance(whole_bytes * 8);
    bits_left -= whole_bytes * 8;
  }

  // Third: Copy the last few bits.
  if (bits_left) {
    WriteSmall(dest_buffer + out_offset_bytes, 0, bits_left,
               ReadSmall(buffer_, offset_bits_, bits_left));
    Advance(bits_left);
  }

  // Return the bit offset to the copied bits.
  return rel_offset_bits;
}

void BitStream::Advance(size_t num_bits) {
  SetOffset(offset_bits_ + num_bits);
}

}  // namespace rex::stream
