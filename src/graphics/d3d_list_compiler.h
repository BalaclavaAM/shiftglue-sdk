#pragma once

// DL-2.1 list compiler state (see d3d_list_compiler.cpp). Private to
// command_processor.cpp and d3d_list_compiler.cpp.

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/register_file.h>

namespace rex::graphics {

struct CommandProcessor::D3dListCompiler {
  enum OpKind : uint32_t {
    kOpRegs,          // PacketWriteRegistersFromMem(first, words + payload, count)
    kOpRegsOne,       // count values into register first
    kOpShaderGuest,   // LoadGuestShader(first = type, payload = address, count = dwords)
    kOpShaderInline,  // LoadInlineShader(first = type, words + payload, count = dwords)
    kOpDraw,          // the draw packet at offset (predicate checked at the run)
    kOpPacket,        // ExecutePacket over the packet at offset
    kOpRegsMem,       // PacketWriteRegistersFromMem(first, physical payload, count)
    kOpBin,           // a bin mask or select packet (first = opcode)
  };
  // A type-3 packet with its predicate bit (type-0 headers use bit 0 for the
  // register index).
  static bool Predicated(uint32_t packet) { return (packet >> 30) == 3 && (packet & 1); }
  struct Op {
    uint32_t kind;
    uint32_t offset;   // the packet header's word offset
    uint32_t first;
    uint32_t count;
    uint32_t payload;  // word offset (or the shader address)
    uint32_t packet;   // the header
  };
  struct Check {
    uint32_t offset;
    uint32_t raw;  // the word as stored (big-endian)
  };
  struct List {
    std::vector<Op> ops;
    std::vector<Check> checks;
    uint32_t runs = 0;
    uint32_t last_swap = 0;
    uint32_t changes = 0;          // structure changes since the last sweep
    uint32_t backoff_until = 0;    // swap; decoded until then
    uint32_t draws = 0;
    bool compiled = false;
    bool uncompilable = false;
  };

  enum Fallback : uint32_t {
    kFirstRun,
    kChanged,
    kBackoff,
    kUncompilable,
    kTooLarge,
    kFallbackCount,
  };
  static constexpr const char* kFallbackName[] = {"first run", "structure changed",
                                                  "volatile backoff", "uncompilable",
                                                  "too large"};
  static constexpr uint32_t kMaxWords = 1u << 16;
  static constexpr uint32_t kSweepSwaps = 600;
  static constexpr uint32_t kVolatileChanges = 4;
  static constexpr uint32_t kBackoffSwaps = 600;

  std::unordered_map<uint64_t, List> lists;
  uint32_t swaps = 0;
  uint32_t sweep_swaps = 0;

  struct Stats {
    uint64_t runs = 0;
    uint64_t replayed = 0;
    uint64_t compiles = 0;
    uint64_t fallback[kFallbackCount] = {};
    uint64_t fallback_draws[kFallbackCount] = {};
    uint64_t ops = 0;
    uint64_t generic_ops = 0;
    uint64_t draws = 0;
    uint64_t bailed = 0;
    uint64_t evicted = 0;
    // Verify.
    uint64_t verified = 0;
    uint64_t verified_draws = 0;
    uint64_t verified_registers = 0;
    uint64_t opaque = 0;
    uint64_t mismatch_lists = 0;
    uint64_t register_mismatches = 0;
    uint64_t draw_count_mismatches = 0;
    uint64_t load_mismatches = 0;
  } stats;
  uint32_t logs = 0;
  // Generic packets met (replay) or that ended the model (verify), by
  // 0x200 (type 1), 0x100 | opcode (predicated type 3) or opcode.
  uint64_t generic[0x201] = {};
  static uint32_t GenericKey(uint32_t packet) {
    if ((packet >> 30) == 1) return 0x200;
    return ((packet & 1) ? 0x100 : 0) | ((packet >> 8) & 0x7F);
  }

  // Verify model.
  struct Load {
    uint32_t type, address, dwords;
  };
  struct Model {
    uint32_t epoch = 0;
    std::unique_ptr<uint32_t[]> values, stamp;
    std::vector<uint32_t> touched;
    // Checkpoints at each modelled draw: touched.size() and the values.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> draws;
    uint32_t modelled_draws = 0;  // draws before the model went opaque
    bool opaque = false;
    std::vector<Load> loads, seen_loads;
    uint32_t seen_draws = 0;
    uint32_t mismatches = 0;
    uint64_t key = 0;
  } model;

  void ModelWrite(uint32_t index, uint32_t value) {
    if (index >= RegisterFile::kRegisterCount) return;
    if (model.stamp[index] != model.epoch) {
      model.stamp[index] = model.epoch;
      model.touched.push_back(index);
    }
    model.values[index] = value;
  }
};

}  // namespace rex::graphics
