#include <rex/filesystem.h>
#include <rex/graphics/fh1_frame_replay.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/types.h>

REXCVAR_DEFINE_STRING(fh1_frame_replay, "", "GPU",
                      "Replay this FH1 frame dump instead of running the title; the result is "
                      "written next to it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(fh1_frame_replay_repeat, 1, "GPU",
                     "Diagnostics: run the replayed frame this many times (a deterministic GPU "
                     "benchmark with fh1_native_gpu_profile, which logs every 600 frames); the "
                     "front buffer is compared after the last")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics {

namespace {

constexpr char kMagic[8] = {'F', 'H', '1', 'F', 'R', 'M', '0', '1'};
constexpr uint32_t kPhysicalSize = 0x20000000;

uint32_t Swap(uint32_t value) { return rex::byte_swap(value); }

template <typename T>
bool Get(std::ifstream& in, T& value) {
  return bool(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

}  // namespace

int RunFh1FrameReplay(CommandProcessor& command_processor, RegisterFile& /*register_file*/,
                      memory::Memory& memory, SharedMemory& shared_memory,
                      const Fh1FrameReplayReadback& read_front_buffer,
                      const std::filesystem::path& requested_path) {
  const std::filesystem::path path = requested_path.empty()
      ? rex::to_path(REXCVAR_GET(fh1_frame_replay)) : requested_path;
  if (path.empty()) return -1;
  std::error_code file_error;
  const auto file_size = std::filesystem::file_size(path, file_error);
  if (file_error || file_size > (1ull << 30)) return 2;
  std::ifstream in(path, std::ios::binary);
  auto remaining = [&]() -> uint64_t {
    const auto position = in.tellg();
    return position < 0 || uint64_t(position) > file_size ? 0 : file_size - uint64_t(position);
  };
  char magic[8] = {};
  uint64_t frame = 0;
  uint32_t register_count = 0;
  if (!in.read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) ||
      !Get(in, frame) || !Get(in, register_count) ||
      register_count != RegisterFile::kRegisterCount) {
    REXGPU_ERROR("FH1 frame replay: {} is not a frame dump", rex::path_to_utf8(path));
    return 2;
  }
  std::vector<uint32_t> registers(register_count);
  in.read(reinterpret_cast<char*>(registers.data()), register_count * 4);
  // Shaders active at the start, loaded like IM_LOAD_IMMEDIATE would.
  std::vector<uint32_t> packets;
  uint32_t shader_count = 0;
  if (!Get(in, shader_count) || shader_count > 2) return 2;
  for (uint32_t i = 0; i < shader_count; ++i) {
    uint32_t type = 0, size = 0;
    if (!Get(in, type) || !Get(in, size) || type > 1 || !size || size > 0x3FFE ||
        uint64_t(size) * 4 > remaining()) return 2;
    std::vector<uint32_t> ucode(size);
    in.read(reinterpret_cast<char*>(ucode.data()), size * 4);
    packets.push_back(Swap((3u << 30) | ((size + 2 - 1) << 16) |
                           (uint32_t(xenos::PM4_IM_LOAD_IMMEDIATE) << 8)));
    packets.push_back(Swap(type));
    packets.push_back(Swap(size));
    for (uint32_t dword : ucode) packets.push_back(Swap(dword));
  }
  uint32_t packet_count = 0;
  if (!Get(in, packet_count) || packet_count > (64u << 20) ||
      uint64_t(packet_count) * 4 > remaining()) return 2;
  const size_t recorded_first = packets.size();
  packets.resize(recorded_first + packet_count);
  in.read(reinterpret_cast<char*>(packets.data() + recorded_first), packet_count * 4);
  for (size_t i = recorded_first; i < packets.size();) {
    const uint32_t packet = Swap(packets[i]);
    const uint32_t type = packet >> 30;
    const uint32_t payload = type == 2 ? 0 : type == 1 ? 2 : ((packet >> 16) & 0x3FFF) + 1;
    if (payload > packets.size() - i - 1) return 2;
    i += 1 + payload;
  }
  uint32_t block_count = 0;
  if (!Get(in, block_count) || block_count > (kPhysicalSize >> 12)) return 2;
  uint64_t block_bytes = 0;
  for (uint32_t i = 0; i < block_count; ++i) {
    uint32_t address = 0, length = 0;
    if (!Get(in, address) || !Get(in, length) || length > kPhysicalSize ||
        block_bytes + length > kPhysicalSize || uint64_t(address) + length > kPhysicalSize ||
        length > remaining()) return 2;
    uint8_t* destination = memory.TranslatePhysical(address);
    if (!destination || uint64_t(address) + length > kPhysicalSize) {
      in.seekg(length, std::ios::cur);
      continue;
    }
    in.read(reinterpret_cast<char*>(destination), length);
    block_bytes += length;
  }
  uint32_t front_address = 0, front_length = 0, width = 0, height = 0, format = 0, tiled = 0;
  Get(in, front_address);
  Get(in, front_length);
  Get(in, width);
  Get(in, height);
  Get(in, format);
  Get(in, tiled);
  if (!in || uint64_t(front_address) + front_length > kPhysicalSize ||
      !front_length || front_length % 4 || !width || !height || width > 8192 || height > 8192 ||
      front_length > remaining()) return 2;
  std::vector<uint8_t> expected(front_length);
  in.read(reinterpret_cast<char*>(expected.data()), front_length);
  if (!in) {
    REXGPU_ERROR("FH1 frame replay: {} is truncated", rex::path_to_utf8(path));
    return 2;
  }

  // Everything written above is new CPU data for the GPU mirror.
  shared_memory.InvalidateAllPages();
  command_processor.RestoreFrameDumpRegisters(registers);
  REXGPU_INFO("FH1 frame replay: frame {} from {} ({} packet dwords, {} blocks, {} MB)", frame,
              rex::path_to_utf8(path), packet_count, block_count, block_bytes >> 20);
  bool executed = true;
  for (int32_t i = 0; executed && i < REXCVAR_GET(fh1_frame_replay_repeat); ++i) {
    executed = command_processor.ExecuteHostPackets(packets.data(), uint32_t(packets.size()));
  }

  // Compare the front buffer the replay resolved with the recorded one.
  std::vector<uint8_t> actual(front_length);
  const bool readback = read_front_buffer(front_address, front_length, actual);
  if (!readback) {
    REXGPU_ERROR("FH1 frame replay: the front buffer could not be read back");
  }
  uint64_t differing_words = 0;
  for (uint32_t i = 0; i + 4 <= front_length; i += 4) {
    differing_words += std::memcmp(expected.data() + i, actual.data() + i, 4) != 0;
  }
  std::filesystem::path result = path;
  result += ".replay.bin";
  std::ofstream(result, std::ios::binary)
      .write(reinterpret_cast<const char*>(actual.data()), actual.size());
  std::filesystem::path json = path;
  json += ".replay.json";
  std::ofstream(json) << "{\"frame\":" << frame << ",\"executed\":" << (executed ? "true" : "false")
                      << ",\"readback\":" << (readback ? "true" : "false")
                      << ",\"packet_dwords\":" << packet_count << ",\"blocks\":" << block_count
                      << ",\"front_address\":" << front_address
                      << ",\"front_bytes\":" << front_length << ",\"width\":" << width
                      << ",\"height\":" << height << ",\"format\":" << format
                      << ",\"tiled\":" << tiled << ",\"differing_words\":" << differing_words
                      << "}\n";
  REXGPU_INFO("FH1 frame replay: done, {} of {} front buffer words differ ({})", differing_words,
              front_length / 4, rex::path_to_utf8(json));
  if (auto inspector = command_processor.frame_inspector()) {
    inspector->SetStatus("Replay: " + std::to_string(differing_words) + " / " +
                         std::to_string(front_length / 4) + " front-buffer words differ" +
                         (readback ? "" : " (readback failed)"));
  }
  return executed && readback ? 0 : 1;
}

}  // namespace rex::graphics
