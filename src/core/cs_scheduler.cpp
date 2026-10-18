#include <rex/cs_scheduler.h>
#include "cs_scheduler_internal.h"
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/session_probe.h>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(cs_schedule_mode, 0, "Diagnostics",
                    "Serialized verification: 0 off, 1 record, 2 replay, 3 timing without log")
    .range(0,3).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(cs_schedule_path, "", "Diagnostics", "Exclusive schedule file for serialized verification")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(cs_schedule_call_budget, 128, "Diagnostics", "Guest calls per execution quantum")
    .range(1,100000).lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::cs_scheduler {
std::atomic<bool> g_enabled{false};
namespace {
struct Header { char magic[8] = {'C','S','S','C','H','0','1',0}; uint64_t epoch=0, count=0; };
struct Grant { uint64_t tag, site, ns; uint32_t kind, reserved=0; };
static_assert(sizeof(Grant)==32);
struct Pending {
  uint64_t site=0, ready_at=0;
  PointKind kind{}; bool ready=false;
  std::vector<thread::WaitHandle*> waits;
};
struct State {
  std::mutex mutex;
  std::map<uint64_t,std::unique_ptr<std::condition_variable>> signals;
  std::map<uint64_t,Pending> actors;
  uint64_t owner=0, previous=0, sequence=0;
  std::atomic<uint64_t> ns{0};
  int mode=0, budget=128;
  Header header;
  Grant next{};
  std::vector<Grant> replay;
  std::map<uint64_t,std::vector<uint64_t>> by_actor;
  std::map<thread::WaitHandle*,thread::WaitHandle*> wait_aliases;
  std::map<uint64_t,uint64_t> actor_aliases;
  std::set<uint64_t> suspended;
  std::map<uint32_t,uint64_t> native_actors;
  bool have_next=false;
  FILE* file=nullptr;
  std::function<void(uint64_t)> timestamp;
  char buffer[1024*1024];
};
State& S() { static auto* s = new State; return *s; }
void Advance(uint64_t delta) {
  const auto before=S().ns.fetch_add(delta);
  if(S().timestamp && before/1000000 != (before+delta)/1000000) S().timestamp((before+delta)/1000000);
}
thread_local uint64_t tag=0, clock_site=0;
thread_local uint32_t no_switch=0, calls=0, repeated_clocks=0;
thread_local std::vector<thread::WaitHandle*> waiting;
[[noreturn]] void Fail(const char* why) {
  auto& s=S();
  const auto path=REXCVAR_GET(cs_schedule_path)+".failure.txt";
  FILE* failure=std::fopen(path.c_str(),"wbx");
  FILE* output=failure?failure:stderr;
  std::fprintf(output,"CS_SCHEDULE_DIVERGENCE %s sequence=%llu owner=%llX expected_tag=%llX site=%llX kind=%u ns=%llu\n",
               why,s.sequence,s.owner,s.next.tag,s.next.site,s.next.kind,s.ns.load());
  for(const auto& [id,p]:s.actors) if(p.ready)
    std::fprintf(output,"CS_SCHEDULE_READY tag=%llX site=%llX kind=%u ready_at=%llu\n",id,p.site,uint32_t(p.kind),p.ready_at);
  std::fflush(output); if(failure) std::fclose(failure);
#if defined(_WIN32)
  // DLL teardown is unsafe with parked verification threads.
  TerminateProcess(GetCurrentProcess(),86);
#endif
  std::_Exit(86);
}
void Finish() {
  auto& s=S(); std::lock_guard lock(s.mutex);
  REXLOG_INFO("CS_SCHEDULE_END mode={} grants={} logical_ns={} max_active=1",s.mode,s.sequence,s.ns.load());
  if(s.mode==2 && s.sequence!=s.header.count) Fail("unconsumed schedule at exit");
  if(s.file) {
    if(s.mode==1) { s.header.count=s.sequence; std::fseek(s.file,0,SEEK_SET); if(std::fwrite(&s.header,sizeof(s.header),1,s.file)!=1) Fail("finish header write"); }
    std::fclose(s.file); s.file=nullptr;
  }
}
void Init() {
  static std::once_flag once;
  std::call_once(once,[]{
    auto& s=S(); s.mode=REXCVAR_GET(cs_schedule_mode);
    s.budget=REXCVAR_GET(cs_schedule_call_budget);
    if(!s.mode) return;
#if !defined(_WIN32)
    Fail("serialized wait virtualization currently requires Windows");
#endif
    s.header.epoch=chrono::Clock::QueryHostSystemTime();
    if(s.mode!=3) {
      auto path=REXCVAR_GET(cs_schedule_path);
      s.file=std::fopen(path.c_str(),s.mode==1?"wbx":"rb");
      if(!s.file) Fail("cannot exclusively create/open schedule");
      std::setvbuf(s.file,s.buffer,_IOFBF,sizeof(s.buffer));
      if(s.mode==1) { if(std::fwrite(&s.header,sizeof(s.header),1,s.file)!=1) Fail("header write"); }
      else {
        const Header expected;
        if(std::fread(&s.header,sizeof(s.header),1,s.file)!=1 ||
           std::memcmp(s.header.magic,expected.magic,8)) Fail("header read/version");
        Grant grant;
        while(const auto bytes=std::fread(&grant,1,sizeof(grant),s.file)) {
          if(bytes!=sizeof(grant) || !grant.tag || grant.reserved || grant.kind>7 ||
             (!s.replay.empty() && grant.ns<s.replay.back().ns)) Fail("invalid/truncated grant");
          s.by_actor[grant.tag].push_back(s.replay.size()); s.replay.push_back(grant);
        }
        if(std::ferror(s.file) || s.replay.size()!=s.header.count) Fail("truncated schedule");
      }
    }
    std::atexit(Finish); g_enabled=true;
    REXLOG_INFO("CS_SCHEDULE_START mode={} budget={} epoch={}",s.mode,s.budget,s.header.epoch);
  });
}
void Choose() {
  auto& s=S();
  if(s.owner) return;
  if(s.actors.empty()) return;
  auto selected=s.actors.end();
  if(s.mode==2) {
    if(!s.have_next) {
      if(s.sequence>=s.replay.size()) Fail("schedule EOF");
      s.next=s.replay[s.sequence];
      s.have_next=true;
    }
    selected=s.actors.find(s.next.tag);
    if(selected==s.actors.end() || !selected->second.ready || s.suspended.contains(selected->first)) return;
    const auto& p=selected->second;
    if(s.next.ns>s.ns.load()) Advance(s.next.ns-s.ns.load());
    if(p.site!=s.next.site || uint32_t(p.kind)!=s.next.kind || s.ns.load()!=s.next.ns)
      Fail("deterministic point mismatch");
    s.have_next=false;
  } else {
    const auto start=s.actors.upper_bound(s.previous);
    auto i=start;
    for(size_t n=0;n<s.actors.size();++n) {
      if(i==s.actors.end()) i=s.actors.begin();
      if(i->second.ready && !s.suspended.contains(i->first) && i->second.ready_at<=s.ns.load()) { selected=i; break; }
      ++i;
    }
    if(selected==s.actors.end()) {
      uint64_t next_time=UINT64_MAX;
      for(const auto& [id,p]:s.actors) if(p.ready && !s.suspended.contains(id)) next_time=std::min(next_time,p.ready_at);
      if(next_time==UINT64_MAX) return;
      if(next_time>s.ns.load()) Advance(next_time-s.ns.load());
      for(auto i=s.actors.begin();i!=s.actors.end();++i) if(i->second.ready && !s.suspended.contains(i->first) && i->second.ready_at<=s.ns.load()) { selected=i; break; }
      if(selected==s.actors.end()) return;
    }
    if(s.mode==1) {
      Grant grant{selected->first,selected->second.site,s.ns.load(),uint32_t(selected->second.kind)};
      if(std::fwrite(&grant,sizeof(grant),1,s.file)!=1) Fail("schedule write");
    }
  }
  s.owner=selected->first; s.previous=s.owner; selected->second.ready=false;
  ++s.sequence;
  // Execution, never elapsed host time, advances the fine logical clock.
  Advance(10000); s.signals.at(s.owner)->notify_one();
}
}
int Mode() { return g_enabled?S().mode:0; }
bool ParkedNativeThread(uint32_t native_id) {
  std::lock_guard lock(S().mutex);
  const auto i=S().native_actors.find(native_id);
  return i!=S().native_actors.end() && S().actors.contains(i->second) &&
         S().actors.at(i->second).ready;
}
bool InActor() { return g_enabled && tag!=0; }
uint64_t Now() { return S().ns.load(); }
uint64_t Epoch() { return S().header.epoch; }
void Point(uint64_t site, PointKind kind, bool force, uint64_t delay_ns) {
  if(!InActor() || (no_switch && !force)) return;
  auto& s=S(); std::unique_lock lock(s.mutex);
  if(kind==PointKind::kAttach) {
    if(s.actors.contains(tag)) Fail("duplicate actor identity");
    s.signals[tag]=std::make_unique<std::condition_variable>();
  } else {
    if(s.owner!=tag) Fail("point without execution permit");
    s.owner=0;
  }
  // Signal notifications wake registered waits promptly; the fallback samples
  // opaque native completions once per logical millisecond.
  if(kind==PointKind::kWait && delay_ns==100000) delay_ns=1000000;
  s.actors[tag]={site,s.ns.load()+delay_ns,kind,true,kind==PointKind::kWait?waiting:std::vector<thread::WaitHandle*>{}};
  Choose();
  while(s.owner!=tag) {
    if(s.signals.at(tag)->wait_for(lock,std::chrono::seconds(30))==std::cv_status::timeout) Fail("no runnable recorded actor");
    Choose();
  }
}
void Detach() {
  if(!InActor()) return;
  auto& s=S(); std::lock_guard lock(s.mutex);
  if(s.owner!=tag) Fail("detach without execution permit");
  s.owner=0; s.actors.erase(tag); s.signals.erase(tag);
#if defined(_WIN32)
  s.native_actors.erase(GetCurrentThreadId());
#endif
  tag=0; no_switch=0; Choose();
}
Actor::Actor(uint64_t id,uint64_t tick_ns) {
  Init(); if(!g_enabled || tag) return;
  if(!id) Fail("zero actor tag");
  previous_tag=session_probe::CurrentTag();
#if defined(_WIN32)
  { std::lock_guard lock(S().mutex); S().native_actors[GetCurrentThreadId()]=id; }
#endif
  if(previous_tag && !(previous_tag>>63)) {
    std::lock_guard lock(S().mutex);
    S().actor_aliases[previous_tag]=id;
    if(S().suspended.contains(previous_tag)) S().suspended.insert(id);
  }
  tag=id; outer=true; session_probe::SetThreadTag(id);
  Point(0,PointKind::kAttach,true);
  Advance(tick_ns);
}
Actor::~Actor() {
  if(outer && tag) { Detach(); session_probe::SetThreadTag(previous_tag); }
}
void Call(uint32_t site) {
  if(InActor() && !no_switch && ++calls>=uint32_t(S().budget)) { calls=0; Point(site,PointKind::kCallBudget); }
}
uint64_t ClockRead(const void* address) {
  if(InActor() && !no_switch) {
    const auto site=reinterpret_cast<uintptr_t>(address);
    repeated_clocks=clock_site==site?repeated_clocks+1:1; clock_site=site;
    if(repeated_clocks==64) { repeated_clocks=0; Point(session_probe::Site(address),PointKind::kPoll,false,100000); }
  }
  return Now();
}
Kernel::Kernel(uint64_t site) {
  if(!InActor()) return;
  active=true;
  if(!no_switch && ++calls>=uint32_t(S().budget)) {
    calls=0; Point(session_probe::Site(reinterpret_cast<const void*>(site)),PointKind::kKernel);
  }
  ++no_switch;
}
Kernel::~Kernel() {
  if(active && InActor()) {
    --no_switch;
  }
}
Callback::Callback():saved(no_switch) { no_switch=0; }
Callback::~Callback() { no_switch=saved; }
void Sleep(uint64_t ns) {
  const auto start=Now();
  do { Point(0,PointKind::kYield,true,ns-(Now()-start)); } while(Now()-start<ns);
}
void SetTimestampWriter(std::function<void(uint64_t)> writer) { S().timestamp=std::move(writer); }
void Wake(uint64_t id) {
  std::lock_guard lock(S().mutex);
  if(auto i=S().actors.find(id);i!=S().actors.end()) i->second.ready_at=0;
}
void Signal(thread::WaitHandle* handle) {
  if(!Enabled()) return;
  std::lock_guard lock(S().mutex);
  for(auto& [id,p]:S().actors)
    if(std::find(p.waits.begin(),p.waits.end(),handle)!=p.waits.end()) p.ready_at=0;
}
void MapWait(thread::WaitHandle* original,thread::WaitHandle* replacement) {
  if(!Enabled()) return;
  std::lock_guard lock(S().mutex);
  if(replacement) S().wait_aliases[original]=replacement;
  else S().wait_aliases.erase(original);
}
thread::WaitHandle* ResolveWait(thread::WaitHandle* original) {
  if(!InActor()) return original;
  std::lock_guard lock(S().mutex);
  auto i=S().wait_aliases.find(original);
  return i==S().wait_aliases.end()?original:i->second;
}
Waiting::Waiting(std::span<thread::WaitHandle* const> handles) {
  if(!InActor()) return;
  saved=std::move(waiting); waiting.assign(handles.begin(),handles.end());
}
Waiting::~Waiting() { if(InActor()) waiting=std::move(saved); }
void ReplayExternalWait() {
  if(!InActor() || Mode()!=2) return;
  for(;;) {
    bool again=false;
    {
      std::lock_guard lock(S().mutex);
      const auto& indices=S().by_actor[tag];
      const auto i=std::lower_bound(indices.begin(),indices.end(),S().sequence);
      again=i!=indices.end() && S().replay[*i].kind==uint32_t(PointKind::kWait);
    }
    if(!again) return;
    Point(0,PointKind::kWait,true,100000);
  }
}
void Flush() {
  if(!Enabled() || Mode()!=1) return;
  std::lock_guard lock(S().mutex);
  fpos_t end;
  if(std::fgetpos(S().file,&end)) Fail("save schedule position");
  S().header.count=S().sequence;
  if(std::fseek(S().file,0,SEEK_SET) ||
     std::fwrite(&S().header,sizeof(Header),1,S().file)!=1 ||
     std::fsetpos(S().file,&end) || std::fflush(S().file)) Fail("flush schedule");
}
void Complete() {
  if(!Enabled()) return;
  // The final GPU-stream comparison proves every preceding PM4 frame was
  // consumed, including the last partial scheduling quantum.
  if(session_probe::ExternalEnabled())
    session_probe::Scalar(session_probe::Kind::kHostHook,UINT64_C(0x435343415054454E),0);
  session_probe::Flush();
  Finish();
}
void Suspend(uint64_t kernel_id) {
  if(!Enabled()) return;
  uint64_t id;
  {
    std::lock_guard lock(S().mutex);
    const auto i=S().actor_aliases.find(kernel_id);
    id=i==S().actor_aliases.end()?kernel_id:i->second;
    S().suspended.insert(id);
  }
  if(id==tag) Point(0,PointKind::kWait,true);
}
void Resume(uint64_t kernel_id) {
  if(!Enabled()) return;
  std::lock_guard lock(S().mutex);
  const auto i=S().actor_aliases.find(kernel_id);
  const auto id=i==S().actor_aliases.end()?kernel_id:i->second;
  S().suspended.erase(kernel_id); S().suspended.erase(id); Choose();
}
void Terminate(uint64_t kernel_id,const std::function<void()>& stop) {
  if(!Enabled()) { stop(); return; }
  std::lock_guard lock(S().mutex);
  const auto i=S().actor_aliases.find(kernel_id);
  const auto id=i==S().actor_aliases.end()?kernel_id:i->second;
  if(S().owner==id) Fail("terminating another permit owner");
  S().actors.erase(id);
  // A parked thread must not be killed while it owns the scheduler mutex.
  stop(); S().actor_aliases.erase(kernel_id);
}
}  // namespace rex::cs_scheduler
