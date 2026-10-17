#include <rex/session_probe.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

REXCVAR_DEFINE_INT32(cs_session_mode, 0, "Diagnostics",
                     "CS-0.1 ordered session boundary pilot: 0 off, 1 record, 2 strict replay")
    .range(0, 2).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(cs_session_path, "", "Diagnostics",
                      "CS-0.1 boundary log path (record refuses an existing file)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::session_probe {
namespace {
static_assert(std::endian::native == std::endian::little);
constexpr char kMagic[8] = {'C','S','0','L','O','G','1',0};
struct Record {
  uint32_t kind, thread;
  uint64_t key, value;
  uint32_t size, reserved;
  uint64_t host_ns;
};
static_assert(sizeof(Record) == 40);
thread_local uint32_t thread_tag = 0;
std::mutex mutex;
FILE* file = nullptr;
uint64_t index = 0;
std::array<char, 1 << 20> io_buffer;

[[noreturn]] void Fail(const char* reason, const Record& expected = {},
                       const Record& actual = {}) {
  // Fail closed; continuing with unrelated reference values could corrupt
  // the private replay state and conceal the first source of divergence.
  std::fprintf(stderr,
      "CS_SESSION_DIVERGENCE index=%llu reason=%s expected=(%u,%08X,%llX,%llX,%u) "
      "actual=(%u,%08X,%llX,%llX,%u)\n",
      static_cast<unsigned long long>(index), reason, expected.kind, expected.thread,
      static_cast<unsigned long long>(expected.key), static_cast<unsigned long long>(expected.value), expected.size,
      actual.kind, actual.thread, static_cast<unsigned long long>(actual.key),
      static_cast<unsigned long long>(actual.value), actual.size);
  std::fflush(stderr);
  REXLOG_ERROR("CS-0.1 replay stopped: {} at boundary {}; "
               "expected=({},{:08X},{:X},{:X},{}) actual=({},{:08X},{:X},{:X},{})",
               reason, index, expected.kind, expected.thread, expected.key,
               expected.value, expected.size, actual.kind, actual.thread,
               actual.key, actual.value, actual.size);
  rex::FlushLogging();
  std::_Exit(86);
}

void Open(int mode) {
  if (file) return;
  const auto path = REXCVAR_GET(cs_session_path);
  if (path.empty()) Fail("empty trace path");
  file = std::fopen(path.c_str(), mode == 1 ? "wbx" : "rb");
  if (!file) Fail(mode == 1 ? "cannot exclusively create trace" : "cannot open reference trace");
  std::setvbuf(file, io_buffer.data(), _IOFBF, io_buffer.size());
  if (mode == 1) {
    if (std::fwrite(kMagic, 1, sizeof(kMagic), file) != sizeof(kMagic)) Fail("write header");
  } else {
    char magic[8];
    if (std::fread(magic, 1, sizeof(magic), file) != sizeof(magic) ||
        std::memcmp(magic, kMagic, sizeof(magic))) Fail("invalid trace header");
  }
}

Record Exchange(Kind kind, uint64_t key, uint64_t value, void* data,
                  uint32_t size, bool replay_value) {
  const int mode = REXCVAR_GET(cs_session_mode);
  if (!mode) return {uint32_t(kind), thread_tag, key, value, size, 0, 0};
  std::lock_guard lock(mutex);
  Open(mode);
  Record actual{uint32_t(kind), thread_tag, key, value, size, 0,
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count())};
  if (size > 4096 || (size && !data)) Fail("invalid payload size", {}, actual);
  if (mode == 1) {
    if (std::fwrite(&actual, sizeof(actual), 1, file) != 1 ||
        (size && std::fwrite(data, 1, size, file) != size)) Fail("write boundary", {}, actual);
    ++index;
    return actual;
  }
  Record expected{};
  if (std::fread(&expected, sizeof(expected), 1, file) != 1) Fail("reference EOF", expected, actual);
  if (expected.kind != actual.kind || expected.thread != actual.thread ||
      expected.key != actual.key || expected.size != size || expected.reserved || size > 4096)
    Fail("boundary order or identity", expected, actual);
  if (!replay_value && expected.value != value) Fail("observed value", expected, actual);
  if (size && std::fread(data, 1, size, file) != size) Fail("truncated payload", expected, actual);
  if (size && Digest(data, size) != expected.value) Fail("payload digest", expected, actual);
  ++index;
  return expected;
}
}  // namespace

bool Enabled() { return REXCVAR_GET(cs_session_mode) != 0; }
void SetThreadTag(uint32_t tag) {
  thread_tag = tag;
  if (Enabled()) Scalar(Kind::kThreadStart, tag, 0);
}
uint64_t Scalar(Kind kind, uint64_t key, uint64_t value, bool replay_value) {
  return Exchange(kind, key, value, nullptr, 0, replay_value).value;
}
void Blob(Kind kind, uint64_t key, void* data, uint32_t size) {
  if (!Enabled()) return;
  if (size > 4096 || (size && !data)) Fail("invalid blob argument");
  Exchange(kind, key, Digest(data, size), data, size, true);
}
void Flush() {
  if (!Enabled()) return;
  std::lock_guard lock(mutex);
  if (file && std::fflush(file)) Fail("flush trace");
}
}  // namespace rex::session_probe
