/**
 * @file        core/sync_census.cpp
 * @brief       NN-3.10 / NN-3.12 probes: global lock sources, write-watch faults
 */

#include <rex/thread/sync_census.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

REXCVAR_DEFINE_INT32(critical_region_census, 0, "Diagnostics",
                     "NN-3.10 probe: count every global critical region acquisition by source "
                     "(object lookups, write watches, guest sections, ...), with contentions "
                     "and blocked time per thread, and log the per-frame averages every this "
                     "many frames (600 is the usual window); 0 is off")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(write_watch_fault_trace, 0, "Diagnostics",
                     "NN-3.12 probe: count write-watch faults apart from announced triggers, "
                     "time the fault path, split write-watch protection changes by owner, and "
                     "log the faulting writers (guest lr), pages and upload kinds every this "
                     "many frames (600 is the usual window); 0 is off")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::thread::census {

std::atomic<bool> write_watch_enabled{false};

namespace {

constexpr size_t kSources = size_t(LockSource::kCount);
constexpr size_t kOwners = size_t(ProtectOwner::kCount);

const char* SourceName(size_t source) {
  static constexpr const char* kNames[] = {
      "other",          "guest_section",    "guest_reservation",     "object_lookup",
      "native_object",  "write_watch_arm",  "write_watch_fault",     "write_watch_announced",
      "shared_memory",  "texture_cache",    "interrupt",             "notify",
  };
  static_assert(std::size(kNames) == kSources, "LockSource names");
  return source < kSources ? kNames[source] : "?";
}

const char* OwnerName(size_t owner) {
  static constexpr const char* kNames[] = {
      "arm_other",     "arm_upload_other", "arm_vertex",    "arm_index",
      "arm_texture",   "arm_memexport",    "arm_gpu_written", "arm_copy_cpu",
      "arm_primitive", "restore_fault",    "restore_announced",
  };
  static_assert(std::size(kNames) == kOwners, "ProtectOwner names");
  return owner < kOwners ? kNames[owner] : "?";
}

const char* KindName(uint8_t kind) {
  switch (kind) {
    case 0: return "other";
    case 1: return "vertex";
    case 2: return "index";
    case 3: return "texture";
    case 4: return "memexport";
    case kPageKindGpuWritten: return "gpu_written";
    case kPageKindNone: return "none";
    default: return "?";
  }
}

// Each thread adds to its own block (one relaxed load and store, as the perf
// counters do); the frame thread sums the blocks.
struct ThreadBlock {
  std::array<std::atomic<uint64_t>, kSources> acquisitions{};
  std::array<std::atomic<uint64_t>, kSources> contentions{};
  std::array<std::atomic<uint64_t>, kSources> blocked_ns{};
  std::atomic<uint64_t> unlocked_reservations{0};
  uint32_t tid = 0;
  ThreadBlock* next = nullptr;
  // Frame thread only: the values at the start of the window.
  std::array<uint64_t, kSources> last_acquisitions{};
  std::array<uint64_t, kSources> last_contentions{};
  std::array<uint64_t, kSources> last_blocked_ns{};
  uint64_t last_unlocked_reservations = 0;
};
std::atomic<ThreadBlock*> g_blocks{nullptr};

ThreadBlock& LocalBlock() {
  // Blocks are never freed: a thread that exits keeps its totals, and the
  // number of threads a title creates is small.
  thread_local ThreadBlock* block = [] {
    auto* b = new ThreadBlock();
    b->tid = rex::thread::current_thread_system_id();
    ThreadBlock* head = g_blocks.load(std::memory_order_relaxed);
    do {
      b->next = head;
    } while (!g_blocks.compare_exchange_weak(head, b, std::memory_order_release,
                                              std::memory_order_relaxed));
    return b;
  }();
  return *block;
}

inline void Add(std::atomic<uint64_t>& counter, uint64_t value) {
  counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

// --- Write-watch state (faults and protects are tens a frame: shared atomics).
struct WriteWatchTotals {
  std::atomic<uint64_t> faults{0};
  std::atomic<uint64_t> faults_unhandled{0};
  std::atomic<uint64_t> fault_ns{0};
  std::atomic<uint64_t> fault_triggers{0};
  std::atomic<uint64_t> fault_triggers_effective{0};
  std::atomic<uint64_t> announced_triggers{0};
  std::atomic<uint64_t> announced_triggers_effective{0};
  std::array<std::atomic<uint64_t>, kOwners> protect_ns{};
  std::array<std::atomic<uint64_t>, kOwners> protect_calls{};
  std::array<std::atomic<uint64_t>, kOwners> protect_pages{};
};
WriteWatchTotals g_ww;

struct FaultRecord {
  uint32_t physical_page;  // physical address >> 12
  uint32_t guest_lr;
  uint32_t tid;
  uint32_t frame;  // window frame index
  uint32_t ns;
  uint8_t kind;
  bool was_valid;
  bool handled;
};
std::mutex g_fault_mutex;
std::vector<FaultRecord> g_faults;
std::atomic<uint32_t> g_ww_window_frame{0};

struct PendingFault {
  bool active = false;
  bool in_trigger = false;
  uint32_t physical_address = 0;
  uint32_t guest_lr = 0;
  uint8_t kind = kPageKindNone;
  bool was_valid = false;
};
thread_local PendingFault t_fault;
thread_local ProtectOwner t_owner = ProtectOwner::kArmOther;

// Frame-thread window state.
int32_t g_lock_frames = 0;
int32_t g_lock_window = 0;
int32_t g_ww_frames = 0;
int32_t g_ww_window = 0;
std::array<uint64_t, 7> g_ww_last{};
std::array<uint64_t, kOwners * 3> g_ww_last_protect{};

std::string ThreadName(uint32_t tid) {
#if defined(__linux__) || defined(__ANDROID__)
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%u/comm", tid);
  if (FILE* f = std::fopen(path, "r")) {
    char name[64] = {};
    size_t n = std::fread(name, 1, sizeof(name) - 1, f);
    std::fclose(f);
    while (n && (name[n - 1] == '\n' || name[n - 1] == ' ')) --n;
    return std::string(name, n);
  }
  return "(exited)";
#else
  (void)tid;
  return "";
#endif
}

void SnapshotLockBaselines() {
  for (ThreadBlock* b = g_blocks.load(std::memory_order_acquire); b; b = b->next) {
    for (size_t s = 0; s < kSources; ++s) {
      b->last_acquisitions[s] = b->acquisitions[s].load(std::memory_order_relaxed);
      b->last_contentions[s] = b->contentions[s].load(std::memory_order_relaxed);
      b->last_blocked_ns[s] = b->blocked_ns[s].load(std::memory_order_relaxed);
    }
    b->last_unlocked_reservations = b->unlocked_reservations.load(std::memory_order_relaxed);
  }
}

void ReportLocks(int32_t frames) {
  struct Row {
    uint32_t tid;
    std::array<uint64_t, kSources> acq{}, cont{}, blocked{};
    uint64_t unlocked = 0;
    uint64_t total_acq = 0, total_cont = 0, total_blocked = 0;
  };
  std::vector<Row> rows;
  Row all{};
  for (ThreadBlock* b = g_blocks.load(std::memory_order_acquire); b; b = b->next) {
    Row r{};
    r.tid = b->tid;
    for (size_t s = 0; s < kSources; ++s) {
      const uint64_t a = b->acquisitions[s].load(std::memory_order_relaxed);
      const uint64_t c = b->contentions[s].load(std::memory_order_relaxed);
      const uint64_t n = b->blocked_ns[s].load(std::memory_order_relaxed);
      r.acq[s] = a - b->last_acquisitions[s];
      r.cont[s] = c - b->last_contentions[s];
      r.blocked[s] = n - b->last_blocked_ns[s];
      b->last_acquisitions[s] = a;
      b->last_contentions[s] = c;
      b->last_blocked_ns[s] = n;
      r.total_acq += r.acq[s];
      r.total_cont += r.cont[s];
      r.total_blocked += r.blocked[s];
      all.acq[s] += r.acq[s];
      all.cont[s] += r.cont[s];
      all.blocked[s] += r.blocked[s];
    }
    const uint64_t u = b->unlocked_reservations.load(std::memory_order_relaxed);
    r.unlocked = u - b->last_unlocked_reservations;
    b->last_unlocked_reservations = u;
    all.unlocked += r.unlocked;
    all.total_acq += r.total_acq;
    all.total_cont += r.total_cont;
    all.total_blocked += r.total_blocked;
    if (r.total_acq || r.unlocked) rows.push_back(r);
  }
  const double f = double(frames);
  std::string sources;
  for (size_t s = 0; s < kSources; ++s) {
    if (!all.acq[s]) continue;
    sources += fmt::format(" {}={:.1f}/{:.2f}/{:.4f}", SourceName(s), double(all.acq[s]) / f,
                           double(all.cont[s]) / f, double(all.blocked[s]) / f / 1e6);
  }
  REXLOG_INFO(
      "Critical region census over {} frames, per frame: acquisitions {:.1f}, contentions "
      "{:.2f}, blocked {:.4f} ms, unlocked reservation sections {:.1f}; by source "
      "(acquisitions/contentions/blocked ms):{}",
      frames, double(all.total_acq) / f, double(all.total_cont) / f,
      double(all.total_blocked) / f / 1e6, double(all.unlocked) / f, sources);
  // The threads that blocked longest (then by acquisitions).
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.total_blocked != b.total_blocked ? a.total_blocked > b.total_blocked
                                              : a.total_acq > b.total_acq;
  });
  const size_t shown = std::min<size_t>(rows.size(), 10);
  for (size_t i = 0; i < shown; ++i) {
    const Row& r = rows[i];
    std::string parts;
    for (size_t s = 0; s < kSources; ++s) {
      if (!r.acq[s]) continue;
      parts += fmt::format(" {}={:.1f}/{:.2f}/{:.4f}", SourceName(s), double(r.acq[s]) / f,
                           double(r.cont[s]) / f, double(r.blocked[s]) / f / 1e6);
    }
    REXLOG_INFO(
        "Critical region census thread {} '{}': acquisitions {:.1f}, contentions {:.2f}, "
        "blocked {:.4f} ms a frame, unlocked reservations {:.1f};{}",
        r.tid, ThreadName(r.tid), double(r.total_acq) / f, double(r.total_cont) / f,
        double(r.total_blocked) / f / 1e6, double(r.unlocked) / f, parts);
  }
}

void SnapshotWriteWatchBaselines() {
  g_ww_last = {g_ww.faults.load(), g_ww.faults_unhandled.load(), g_ww.fault_ns.load(),
               g_ww.fault_triggers.load(), g_ww.fault_triggers_effective.load(),
               g_ww.announced_triggers.load(), g_ww.announced_triggers_effective.load()};
  for (size_t o = 0; o < kOwners; ++o) {
    g_ww_last_protect[o * 3 + 0] = g_ww.protect_ns[o].load();
    g_ww_last_protect[o * 3 + 1] = g_ww.protect_calls[o].load();
    g_ww_last_protect[o * 3 + 2] = g_ww.protect_pages[o].load();
  }
  std::lock_guard<std::mutex> lock(g_fault_mutex);
  g_faults.clear();
}

void ReportWriteWatch(int32_t frames) {
  const std::array<uint64_t, 7> now = {
      g_ww.faults.load(), g_ww.faults_unhandled.load(), g_ww.fault_ns.load(),
      g_ww.fault_triggers.load(), g_ww.fault_triggers_effective.load(),
      g_ww.announced_triggers.load(), g_ww.announced_triggers_effective.load()};
  std::array<uint64_t, 7> d;
  for (size_t i = 0; i < d.size(); ++i) d[i] = now[i] - g_ww_last[i];
  std::array<uint64_t, kOwners * 3> p;
  for (size_t o = 0; o < kOwners; ++o) {
    const uint64_t v[3] = {g_ww.protect_ns[o].load(), g_ww.protect_calls[o].load(),
                           g_ww.protect_pages[o].load()};
    for (size_t k = 0; k < 3; ++k) {
      p[o * 3 + k] = v[k] - g_ww_last_protect[o * 3 + k];
    }
  }
  std::vector<FaultRecord> faults;
  {
    std::lock_guard<std::mutex> lock(g_fault_mutex);
    faults.swap(g_faults);
  }
  SnapshotWriteWatchBaselines();

  const double f = double(frames);
  std::string protects;
  uint64_t protect_ns_total = 0;
  for (size_t o = 0; o < kOwners; ++o) {
    protect_ns_total += p[o * 3];
    if (!p[o * 3 + 1]) continue;
    protects += fmt::format(" {}={:.4f}ms/{:.1f}/{:.1f}", OwnerName(o), double(p[o * 3]) / f / 1e6,
                            double(p[o * 3 + 1]) / f, double(p[o * 3 + 2]) / f);
  }
  REXLOG_INFO(
      "Write-watch census over {} frames, per frame: faults {:.2f} ({:.2f} unhandled, fault path "
      "{:.4f} ms, {:.1f} us each), fault triggers {:.2f} ({:.2f} effective), announced triggers "
      "{:.2f} ({:.2f} effective); protect {:.4f} ms by owner (ms/calls/pages):{}",
      frames, double(d[0]) / f, double(d[1]) / f, double(d[2]) / f / 1e6,
      d[0] ? double(d[2]) / double(d[0]) / 1e3 : 0.0, double(d[3]) / f, double(d[4]) / f,
      double(d[5]) / f, double(d[6]) / f, double(protect_ns_total) / f / 1e6, protects);
  if (faults.empty()) return;

  // Pages: faults, distinct frames with a fault, the kind seen last.
  struct Page {
    uint32_t faults = 0;
    uint32_t frames = 0;
    uint32_t last_frame = UINT32_MAX;
    uint8_t kind = kPageKindNone;
  };
  std::unordered_map<uint32_t, Page> pages;
  for (const FaultRecord& r : faults) {
    Page& page = pages[r.physical_page];
    ++page.faults;
    if (page.last_frame != r.frame) {
      ++page.frames;
      page.last_frame = r.frame;
    }
    page.kind = r.kind;
  }
  const uint32_t recurrent_frames = uint32_t((uint64_t(frames) * 9 + 9) / 10);
  uint32_t recurrent_pages = 0;
  uint64_t recurrent_faults = 0;
  for (const auto& [number, page] : pages) {
    if (page.frames >= recurrent_frames) {
      ++recurrent_pages;
      recurrent_faults += page.faults;
    }
  }
  std::array<uint64_t, 256> kinds{};
  uint64_t was_valid = 0;
  for (const FaultRecord& r : faults) {
    ++kinds[r.kind];
    was_valid += r.was_valid;
  }
  std::string kind_text;
  for (size_t k = 0; k < kinds.size(); ++k) {
    if (kinds[k]) {
      kind_text += fmt::format(" {}={:.2f}", KindName(uint8_t(k)), double(kinds[k]) / f);
    }
  }
  REXLOG_INFO(
      "Write-watch faults over {} frames: {} records, {} distinct pages, {} recurrent (faulted in "
      ">= 90 % of the frames) holding {:.1f} % of the faults; page valid before the fault "
      "{:.1f} %; by upload kind a frame:{}",
      frames, faults.size(), pages.size(), recurrent_pages,
      100.0 * double(recurrent_faults) / double(faults.size()),
      100.0 * double(was_valid) / double(faults.size()), kind_text);

  // Writers: the guest lr of the faulting thread.
  struct Writer {
    uint64_t faults = 0;
    uint64_t ns = 0;
    std::unordered_map<uint32_t, uint32_t> pages;  // page -> faults
    std::array<uint32_t, 256> kinds{};
    uint32_t tid = 0;
  };
  std::unordered_map<uint32_t, Writer> writers;
  for (const FaultRecord& r : faults) {
    Writer& w = writers[r.guest_lr];
    ++w.faults;
    w.ns += r.ns;
    ++w.pages[r.physical_page];
    ++w.kinds[r.kind];
    w.tid = r.tid;
  }
  std::vector<std::pair<uint32_t, const Writer*>> order;
  order.reserve(writers.size());
  for (const auto& [lr, w] : writers) order.emplace_back(lr, &w);
  std::sort(order.begin(), order.end(),
            [](const auto& a, const auto& b) { return a.second->faults > b.second->faults; });
  const size_t shown = std::min<size_t>(order.size(), 12);
  for (size_t i = 0; i < shown; ++i) {
    const Writer& w = *order[i].second;
    uint32_t w_recurrent = 0;
    uint64_t w_first_page = UINT64_MAX, w_last_page = 0;
    for (const auto& [number, count] : w.pages) {
      if (pages[number].frames >= recurrent_frames) ++w_recurrent;
      w_first_page = std::min<uint64_t>(w_first_page, number);
      w_last_page = std::max<uint64_t>(w_last_page, number);
    }
    uint8_t top_kind = 0;
    for (size_t k = 1; k < w.kinds.size(); ++k) {
      if (w.kinds[k] > w.kinds[top_kind]) top_kind = uint8_t(k);
    }
    REXLOG_INFO(
        "Write-watch fault writer lr 0x{:08X} (thread {} '{}'): {:.2f} faults a frame, {:.1f} us "
        "each, {} pages ({} recurrent) in physical 0x{:08X}-0x{:08X}, mostly {} ({:.0f} %)",
        order[i].first, w.tid, ThreadName(w.tid), double(w.faults) / f,
        double(w.ns) / double(w.faults) / 1e3, w.pages.size(), w_recurrent,
        uint32_t(w_first_page << 12), uint32_t((w_last_page << 12) + 0xFFF), KindName(top_kind),
        100.0 * double(w.kinds[top_kind]) / double(w.faults));
  }
}

}  // namespace

void Frame() {
  const int32_t lock_frames = std::max<int32_t>(0, REXCVAR_GET(critical_region_census));
  if (lock_frames != g_lock_frames) {
    g_lock_frames = lock_frames;
    g_lock_window = 0;
    if (lock_frames) SnapshotLockBaselines();
    global_critical_region::census_enabled_.store(lock_frames != 0, std::memory_order_relaxed);
  } else if (lock_frames && ++g_lock_window >= lock_frames) {
    ReportLocks(g_lock_window);
    g_lock_window = 0;
  }

  const int32_t ww_frames = std::max<int32_t>(0, REXCVAR_GET(write_watch_fault_trace));
  if (ww_frames != g_ww_frames) {
    g_ww_frames = ww_frames;
    g_ww_window = 0;
    g_ww_window_frame.store(0, std::memory_order_relaxed);
    if (ww_frames) SnapshotWriteWatchBaselines();
    write_watch_enabled.store(ww_frames != 0, std::memory_order_relaxed);
  } else if (ww_frames) {
    if (++g_ww_window >= ww_frames) {
      ReportWriteWatch(g_ww_window);
      g_ww_window = 0;
    }
    g_ww_window_frame.store(uint32_t(g_ww_window), std::memory_order_relaxed);
  }
}

void NoteLock(LockSource source, bool contended, int64_t waited_ns) {
  ThreadBlock& b = LocalBlock();
  const size_t s = std::min(size_t(source), kSources - 1);
  Add(b.acquisitions[s], 1);
  if (contended) {
    Add(b.contentions[s], 1);
    Add(b.blocked_ns[s], uint64_t(std::max<int64_t>(waited_ns, 0)));
  }
}

void NoteUnlockedReservation() { Add(LocalBlock().unlocked_reservations, 1); }

ProtectOwner ProtectOwnerScope::Exchange(ProtectOwner owner) {
  const ProtectOwner previous = t_owner;
  t_owner = owner;
  return previous;
}

void NoteProtect(bool restore, int64_t ns, uint32_t pages) {
  ProtectOwner owner = t_owner;
  if (restore) {
    owner = t_fault.active ? ProtectOwner::kRestoreFault : ProtectOwner::kRestoreAnnounced;
  }
  const size_t o = size_t(owner);
  g_ww.protect_ns[o].fetch_add(uint64_t(std::max<int64_t>(ns, 0)), std::memory_order_relaxed);
  g_ww.protect_calls[o].fetch_add(1, std::memory_order_relaxed);
  g_ww.protect_pages[o].fetch_add(pages, std::memory_order_relaxed);
}

void BeginFault(uint32_t physical_address, uint32_t guest_lr) {
  t_fault.active = true;
  t_fault.in_trigger = false;
  t_fault.physical_address = physical_address;
  t_fault.guest_lr = guest_lr;
  t_fault.kind = kPageKindNone;
  t_fault.was_valid = false;
}

void EndFault(bool handled, int64_t ns) {
  if (!t_fault.active) return;
  t_fault.active = false;
  g_ww.faults.fetch_add(1, std::memory_order_relaxed);
  if (!handled) g_ww.faults_unhandled.fetch_add(1, std::memory_order_relaxed);
  g_ww.fault_ns.fetch_add(uint64_t(std::max<int64_t>(ns, 0)), std::memory_order_relaxed);
  FaultRecord record;
  record.physical_page = t_fault.physical_address >> 12;
  record.guest_lr = t_fault.guest_lr;
  record.tid = rex::thread::current_thread_system_id();
  record.frame = g_ww_window_frame.load(std::memory_order_relaxed);
  record.ns = uint32_t(std::min<int64_t>(std::max<int64_t>(ns, 0), INT32_MAX));
  record.kind = t_fault.kind;
  record.was_valid = t_fault.was_valid;
  record.handled = handled;
  std::lock_guard<std::mutex> lock(g_fault_mutex);
  // A bound against a window that never ends (a frame thread stopped).
  if (g_faults.size() < (size_t(1) << 20)) g_faults.push_back(record);
}

void NoteFaultPage(uint8_t kind, bool was_valid) {
  if (!t_fault.active) return;
  t_fault.kind = kind;
  t_fault.was_valid = was_valid;
}

void NoteTrigger(bool effective) {
  if (t_fault.active) {
    g_ww.fault_triggers.fetch_add(1, std::memory_order_relaxed);
    if (effective) g_ww.fault_triggers_effective.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_ww.announced_triggers.fetch_add(1, std::memory_order_relaxed);
    if (effective) g_ww.announced_triggers_effective.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace rex::thread::census
