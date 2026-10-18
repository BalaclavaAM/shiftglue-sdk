#include <rex/session_probe.h>
#include <rex/cs_scheduler.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

REXCVAR_DEFINE_INT32(cs_session_mode, 0, "Diagnostics",
                     "CS-0.1 per-thread session probe: 0 off, 1 record, 2 replay")
    .range(0, 2).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// The runtime creates a UI device before GPU plugin settings are registered.
REXCVAR_DEFINE_BOOL(gpu_async_fence_probe, false, "GPU",
                    "CS-0.4: create the Vulkan timeline completion worker")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(cs_session_path, "", "Diagnostics",
                      "CS-0.1 per-thread trace directory (record refuses an existing path)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::session_probe {
namespace {
static_assert(std::endian::native == std::endian::little);
constexpr char kMagic[8] = {'C','S','0','L','O','G','2',0};
struct Record {
  uint32_t kind, size;
  uint64_t thread, sequence, key, value, host_ns;
};
static_assert(sizeof(Record) == 48);
struct Stream {
  FILE* file = nullptr;
  uint64_t sequence = 0;
  std::array<char, 64 * 1024> buffer;
  std::recursive_mutex mutex;
  bool started = false;
  ~Stream() { if (file) std::fclose(file); }
};
thread_local uint64_t thread_tag = 0;
thread_local Stream* current_stream = nullptr;
thread_local uint32_t suppression_depth = 0;
std::mutex streams_mutex;
std::map<uint64_t, std::unique_ptr<Stream>> streams;
std::once_flag root_once;
std::filesystem::path root;
std::chrono::steady_clock::time_point epoch;
uint32_t main_native_tid = 0;
std::atomic<uint64_t> matched_frames{0};
std::atomic<uint64_t> recorded_frames{0};
std::map<uint64_t, uint64_t> lock_tickets, lock_turns;
std::mutex grant_mutex;
std::condition_variable grant_wake;
std::mutex observations_mutex;
std::map<uint32_t, Record> last_stores;

[[noreturn]] void Fail(const char* reason, const Record& expected = {},
                       const Record& actual = {}) {
  REXLOG_ERROR("CS_SESSION_DIVERGENCE {} tag={:016X} seq={} matched_frames={} "
               "expected=({},{:016X},{},{:X},{:X},{}) actual=({},{:016X},{},{:X},{:X},{})",
               reason, thread_tag, current_stream ? current_stream->sequence : 0, matched_frames.load(),
               expected.kind, expected.thread, expected.sequence, expected.key,
               expected.value, expected.size, actual.kind, actual.thread,
               actual.sequence, actual.key, actual.value, actual.size);
  if (actual.kind == uint32_t(Kind::kGuestMemory)) {
    std::lock_guard lock(observations_mutex);
    const auto address = uint32_t(actual.value >> 32) + uint32_t(actual.key & 0x7FFFFFFF);
    if (const auto found = last_stores.find(address); found != last_stores.end()) {
      const auto& store = found->second;
      REXLOG_ERROR("CS_SESSION_LAST_STORE addr={:08X} thread={:016X} seq={} site={:016X} value={:X} actual_ns={} reference_read_ns={}",
                   address, store.thread, store.sequence, store.key, store.value, store.host_ns, expected.host_ns);
    }
  }
#if defined(_WIN32)
  void* frames[24];
  const auto count = CaptureStackBackTrace(0, 24, frames, nullptr);
  for (unsigned i = 0; i < count; ++i)
    REXLOG_ERROR("CS_SESSION_STACK {} {:016X}", i, Site(frames[i]));
#endif
  rex::FlushLogging();
#if defined(_WIN32)
  if(cs_scheduler::Enabled()) TerminateProcess(GetCurrentProcess(),86);
#endif
  std::_Exit(86);
}

Stream& Open(int mode) {
  if (current_stream) return *current_stream;
  std::call_once(root_once, [mode]() {
    root = REXCVAR_GET(cs_session_path);
    epoch = std::chrono::steady_clock::now();
    std::error_code ec;
    if (root.empty()) Fail("empty trace directory");
    if (mode == 1) {
      if (!std::filesystem::create_directory(root, ec)) Fail("cannot exclusively create trace directory");
    } else if (!std::filesystem::is_directory(root, ec)) Fail("missing trace directory");
  });
  std::lock_guard lock(streams_mutex);
  if (!thread_tag) {
    const auto tid = rex::thread::current_thread_system_id();
    if (!main_native_tid) main_native_tid = tid;
    if (main_native_tid != tid) Fail("capture incomplete: untagged host thread");
  }
  auto& stream = streams[thread_tag];
  if (!stream) {
    stream = std::make_unique<Stream>();
    char name[32];
    std::snprintf(name, sizeof(name), "%016llX.bin", static_cast<unsigned long long>(thread_tag));
    auto path = (root / name).string();
    stream->file = std::fopen(path.c_str(), mode == 1 ? "wbx" : "rb");
    if (!stream->file) Fail("cannot open per-thread stream");
    std::setvbuf(stream->file, stream->buffer.data(), _IOFBF, stream->buffer.size());
    if (mode == 1) {
      if (std::fwrite(kMagic, 1, 8, stream->file) != 8 ||
          std::fwrite(&thread_tag, 8, 1, stream->file) != 1) Fail("write stream header");
    } else {
      char magic[8]; uint64_t tag = 0;
      if (std::fread(magic, 1, 8, stream->file) != 8 || std::memcmp(magic,kMagic,8) ||
          std::fread(&tag,8,1,stream->file) != 1 || tag != thread_tag) Fail("invalid stream header");
    }
  }
  current_stream = stream.get();
  return *current_stream;
}

Record Exchange(Kind kind, uint64_t key, uint64_t value, void* data,
                uint32_t size, bool replay_value) {
  const int mode = REXCVAR_GET(cs_session_mode);
  if (!mode) return {uint32_t(kind), size, thread_tag, 0, key, value, 0};
  if (cs_scheduler::Enabled()) {
    switch (kind) {
      case Kind::kInputResult: case Kind::kInputState: case Kind::kInputCapsResult:
      case Kind::kInputCapsData: case Kind::kInputKeyResult: case Kind::kInputKeyData:
      case Kind::kIoRead: case Kind::kIoWrite: case Kind::kIoData:
      case Kind::kIoReadRequest: case Kind::kIoReadData: case Kind::kIoResult: case Kind::kIoMetadata:
      case Kind::kPm4Frame: case Kind::kPm4Words: case Kind::kAudioPump: case Kind::kHostHook: break;
      default: return {uint32_t(kind),size,thread_tag,0,key,value,0};
    }
  }
  if (!thread_tag && main_native_tid && main_native_tid != rex::thread::current_thread_system_id())
    REXLOG_ERROR("CS_SESSION_UNTAGGED kind={} key={:016X}", uint32_t(kind), key);
  auto& stream = Open(mode);
  std::lock_guard stream_lock(stream.mutex);
  Record actual{uint32_t(kind),size,thread_tag,stream.sequence,key,value,
      cs_scheduler::Enabled() ? cs_scheduler::Now() :
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-epoch).count())};
  if (size > 4096 || (size && !data)) Fail("invalid payload size",{},actual);
  if (kind == Kind::kGuestMemory && (key & (UINT64_C(1) << 31))) {
    std::lock_guard lock(observations_mutex);
    last_stores[uint32_t(value >> 32) + uint32_t(key & 0x7FFFFFFF)] = actual;
  }
  if (mode == 1) {
    if (std::fwrite(&actual,sizeof(actual),1,stream.file)!=1 ||
        (size && std::fwrite(data,1,size,stream.file)!=size)) Fail("write boundary",{},actual);
    ++stream.sequence;
    if(kind==Kind::kPm4Words) recorded_frames.fetch_add(1);
    if(kind==Kind::kHostHook && key==UINT64_C(0x435343415054454E))
      REXLOG_INFO("CS_SESSION_CAPTURE_END recorded_frames={} matched_frames=0",recorded_frames.load());
    return actual;
  }
  Record expected{};
  if (std::fread(&expected,sizeof(expected),1,stream.file)!=1) Fail("reference EOF",expected,actual);
  if (expected.kind!=actual.kind || expected.thread!=thread_tag ||
      expected.sequence!=stream.sequence || expected.key!=key || expected.size!=size)
    Fail("per-thread boundary identity",expected,actual);
  if (!replay_value && expected.value!=value) Fail("observed value",expected,actual);
  if (size && std::fread(data,1,size,stream.file)!=size) Fail("truncated payload",expected,actual);
  if (size && Digest(data,size)!=expected.value) Fail("payload digest",expected,actual);
  ++stream.sequence;
  if (!cs_scheduler::Enabled() && (replay_value || kind==Kind::kSyncResult || kind==Kind::kGpuFence || kind==Kind::kGpuInterrupt))
    std::this_thread::sleep_until(epoch+std::chrono::nanoseconds(expected.host_ns));
  if (kind==Kind::kPm4Words) {
    const auto matched=matched_frames.fetch_add(1)+1;
    if (matched%600==0) REXLOG_INFO("CS_SESSION_MATCHED frames={} logical_ns={}",matched,cs_scheduler::Now());
  }
  if(kind==Kind::kHostHook && key==UINT64_C(0x435343415054454E))
    REXLOG_INFO("CS_SESSION_CAPTURE_END recorded_frames=0 matched_frames={}",matched_frames.load());
  return expected;
}
}  // namespace

bool ExternalEnabled() { return REXCVAR_GET(cs_session_mode)!=0 && !suppression_depth; }
bool Enabled() { return !cs_scheduler::Enabled() && ExternalEnabled(); }
bool Active() { return !cs_scheduler::Enabled() && REXCVAR_GET(cs_session_mode)!=0; }
bool Replaying() { return REXCVAR_GET(cs_session_mode)==2; }
void Require(bool condition, const char* reason) { if (!condition) Fail(reason); }
Suppress::Suppress() { ++suppression_depth; }
Suppress::~Suppress() { --suppression_depth; }
LockGrant::LockGrant(uint64_t value) : key(value), active(Enabled()) {
  if (active && Replaying()) {
    auto ticket = Scalar(Kind::kLockGrant, key, 0, true);
    std::unique_lock lock(grant_mutex);
    grant_wake.wait(lock, [this, ticket]() { return lock_turns[key] == ticket; });
  }
}
void LockGrant::Acquired() {
  if (!active) return;
  if (!Replaying()) {
    uint64_t ticket;
    { std::lock_guard lock(grant_mutex); ticket = lock_tickets[key]++; }
    Scalar(Kind::kLockGrant, key, ticket);
  }
  else {
    { std::lock_guard lock(grant_mutex); ++lock_turns[key]; }
    grant_wake.notify_all();
  }
}
uint64_t CurrentTag() { return thread_tag; }
void SetThreadTag(uint64_t tag, uint32_t entry) {
  if (thread_tag==tag) return;
  thread_tag=tag;current_stream=nullptr;
  if (ExternalEnabled() && !cs_scheduler::Enabled()) {
    auto& stream=Open(REXCVAR_GET(cs_session_mode));
    std::lock_guard lock(stream.mutex);
    if (!stream.started) {
      stream.started=true;
      Scalar(Kind::kThreadStart,entry,tag);
    }
  }
}
void SetHostRole(uint64_t role) { SetThreadTag(UINT64_C(0x8000000000000000)|role); }
uint64_t Scalar(Kind kind, uint64_t key, uint64_t value, bool replay_value) {
  return Exchange(kind,key,value,nullptr,0,replay_value).value;
}
void Blob(Kind kind, uint64_t key, void* data, uint32_t size) {
  if (!ExternalEnabled()) return;
  if (size>4096 || (size && !data)) Fail("invalid blob argument");
  Exchange(kind,key,Digest(data,size),data,size,true);
}
void Bytes(Kind kind, uint64_t key, void* data, uint32_t size) {
  auto* bytes=static_cast<uint8_t*>(data);
  for (uint32_t offset=0;offset<size;) {
    const auto n=std::min(uint32_t(4096),size-offset);
    Blob(kind,key^offset,bytes+offset,n);offset+=n;
  }
}
void Delivery(Kind kind, uint64_t key, uint64_t value) {
  if (!Enabled()) return;
  auto event=Exchange(kind,key,value,nullptr,0,false);
  if (Replaying()) std::this_thread::sleep_until(epoch+std::chrono::nanoseconds(event.host_ns));
}
std::optional<uint32_t> PeekWaitResult(uint64_t key) {
  if (!Enabled() || !Replaying()) return std::nullopt;
  auto& stream=Open(2);fpos_t position;
  std::lock_guard stream_lock(stream.mutex);
  if (std::fgetpos(stream.file,&position)) Fail("save wait lookahead position");
  Record record{};uint32_t depth=0;bool found=false;
  while (std::fread(&record,sizeof(record),1,stream.file)==1) {
    if (record.size>4096 || record.thread!=thread_tag) Fail("invalid wait lookahead");
    if (record.kind==uint32_t(Kind::kSyncRequest)) ++depth;
    if (record.kind==uint32_t(Kind::kSyncResult)) {
      if (!depth) { found=record.key==key;break; }
      --depth;
    }
    if (record.size && std::fseek(stream.file,record.size,SEEK_CUR)) Fail("skip wait payload");
  }
  if (std::fsetpos(stream.file,&position)) Fail("restore wait lookahead position");
  // A thread may still be waiting when the bounded recording ends.
  if (!found && !std::ferror(stream.file)) return std::nullopt;
  if (!found) Fail("missing wait result");
  return uint32_t(record.value);
}
uint64_t Site(const void* return_address) {
  if (!ExternalEnabled() && !cs_scheduler::Enabled()) return 0;
  thread_local std::map<const void*,uint64_t> cached;
  if (const auto found=cached.find(return_address); found!=cached.end()) return found->second;
  uintptr_t base=0; const char* name="unknown";
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(return_address,&info,sizeof(info))) base=reinterpret_cast<uintptr_t>(info.AllocationBase);
  char path[MAX_PATH];
  if (base && GetModuleFileNameA(reinterpret_cast<HMODULE>(base),path,MAX_PATH)) name=path;
#else
  Dl_info info{};
  if (dladdr(return_address,&info)) { base=reinterpret_cast<uintptr_t>(info.dli_fbase); name=info.dli_fname; }
#endif
  for (const char* c=name;*c;++c) if (*c=='/' || *c=='\\') name=c+1;
  const auto value=(uint64_t(uint32_t(Digest(name,std::strlen(name))))<<32)|uint32_t(reinterpret_cast<uintptr_t>(return_address)-base);
  cached.emplace(return_address,value); return value;
}
void Flush() {
  cs_scheduler::Flush();
  if (!ExternalEnabled() || Replaying()) return;
  std::lock_guard lock(streams_mutex);
  for (const auto& [tag,stream]:streams) {
    std::lock_guard stream_lock(stream->mutex);
    if (stream->file && std::fflush(stream->file)) Fail("flush stream");
  }
}
}  // namespace rex::session_probe
