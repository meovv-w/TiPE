#include "tipe/learning.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <charconv>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <stdexcept>
#include <unordered_set>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
namespace fs=std::filesystem;
namespace tipe { namespace {
uint64_t nowms(){using namespace std::chrono;return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();}
uint64_t thread_cpu_us(){timespec ts{}; if(::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts)!=0)return 0; return static_cast<uint64_t>(ts.tv_sec)*1000000ULL+static_cast<uint64_t>(ts.tv_nsec)/1000ULL;}
#ifdef TIPE_TEST_CRASH_INJECTION
void crash_point(const char *point) {
  const char *wanted = std::getenv("TIPE_LEARNER_TEST_CRASH_POINT");
  if (wanted && std::string_view(wanted) == point) ::kill(::getpid(), SIGKILL);
}
#else
void crash_point(const char *) {}
#endif
uint64_t fnv(const std::string&s){uint64_t h=1469598103934665603ULL;for(unsigned char c:s){h^=c;h*=1099511628211ULL;}return h;}
std::string clean(std::string s){for(char&c:s)if(c=='\t'||c=='\n'||c=='\r')c=' ';return s;}
// Candidate navigation is useful telemetry, but repeated identical moves in
// one short input batch carry no additional learning signal.  Coalesce only
// adjacent moves with the same direction/key, session, context and privacy
// class.  Keeping the latest event preserves the final timestamp while the
// strict adjacency/type checks leave key, selection, commit and correction
// evidence untouched.
void coalesce_batch(std::deque<Event>& batch) {
  if (batch.size() < 2) return;
  std::deque<Event> compact;
  compact.clear();
  for (auto &event : batch) {
    if (!compact.empty()) {
      auto &previous = compact.back();
      if (event.type == EventType::CandidateMove &&
          previous.type == EventType::CandidateMove &&
          event.key == previous.key &&
          event.session == previous.session &&
          event.context == previous.context &&
          event.sensitive == previous.sensitive) {
        previous = std::move(event);
        continue;
      }
    }
    compact.push_back(std::move(event));
  }
  batch.swap(compact);
}
struct Lock { int fd{-1}; explicit Lock(const std::string&p,int op=LOCK_EX|LOCK_NB){fd=::open(p.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);if(fd>=0&&::flock(fd,op)!=0){::close(fd);fd=-1;}} ~Lock(){if(fd>=0){::flock(fd,LOCK_UN);::close(fd);}} explicit operator bool()const{return fd>=0;} };
std::string default_dir(){if(const char*h=getenv("XDG_STATE_HOME");h&&*h)return std::string(h)+"/tipe";if(const char*h=getenv("HOME");h&&*h)return std::string(h)+"/.local/state/tipe";return ".tipe";}
void fsync_file(const std::string&p){int fd=::open(p.c_str(),O_RDONLY|O_CLOEXEC);if(fd>=0){::fsync(fd);::close(fd);}}
void fsync_directory(const std::string&p){int fd=::open(p.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd>=0){::fsync(fd);::close(fd);}}
void remove_legacy_generations(const std::string&d){std::error_code e;for(auto const&x:fs::directory_iterator(d,e)){auto name=x.path().filename().string();if(name.rfind("model-",0)==0)fs::remove(x.path(),e);}}
void remove_stale_temps(const std::string &d) {
  // These names are private implementation temporaries.  Removing only this
  // allowlist on startup makes an interrupted fsync/rename recoverable while
  // leaving user exports and unrelated files untouched.
  static constexpr const char *names[] = {
      "model.current.tmp", "model.previous.tmp", "state.current.tmp",
      "control.current.tmp"};
  std::error_code e;
  for (const auto *name : names) fs::remove(fs::path(d) / name, e);
}
void remove_models(const std::string&d){std::error_code e;fs::remove(d+"/model.current",e);fs::remove(d+"/model.previous",e);fs::remove(d+"/model.current.tmp",e);fs::remove(d+"/model.previous.tmp",e);for(auto const&x:fs::directory_iterator(d,e))if(x.path().filename().string().rfind("model-",0)==0)fs::remove(x.path(),e);}
uint64_t correction_support(const std::unordered_map<std::string, correction::RuleStore>& all) {
  uint64_t total = 0;
  for (const auto &[ctx, store] : all)
    for (const auto &rule : store.rules())
      total = std::numeric_limits<uint64_t>::max() - total < rule.support
                ? std::numeric_limits<uint64_t>::max() : total + rule.support;
  return total;
}
}
Learner::Learner(std::string d,LearnerMode mode):dir_(std::move(d)){
 if(dir_.empty())dir_=default_dir();std::error_code e;fs::create_directories(dir_,e);::chmod(dir_.c_str(),0700);model_settings_=std::make_unique<ModelSettings>(dir_);
 CloudTransportOptions cloud_options;
 if(const char *endpoint=std::getenv("TIPE_CLOUD_ENDPOINT"); endpoint && *endpoint) cloud_options.endpoint=endpoint;
 if(const char *enabled=std::getenv("TIPE_CLOUD_ENABLED"); enabled && std::string(enabled)=="1") cloud_options.enabled=true;
 if(const char *key=std::getenv("TIPE_CLOUD_API_KEY"); key && *key) { cloud_options.include_api_key=true; cloud_options.api_key_path=key; }
 else if(const auto path=model_settings_->cloud_api_key_path();!path.empty()) {
   cloud_options.include_api_key=true;cloud_options.api_key_path=path;
 }
 if(const char *ca=std::getenv("TIPE_CLOUD_CA"); ca && *ca) cloud_options.ca_certificate_path=ca;
 const char *consent_env=std::getenv("TIPE_CLOUD_CONSENT");
 const bool cloud_consent = consent_env && std::string(consent_env)=="1";
 cloud_options_=cloud_options;
 snapshot_.store(std::make_shared<const LearningModel>());load();
 refresh_disk_bytes();
 Control c=read_control();paused_=c.paused;enabled_=c.enabled; gates_revision_=c.revision; requested_revision_=c.revision;
 if(auto st=read_state()){ack_=st->ack;clear_ack_=st->clear_ack;rollback_ack_=st->rollback_ack;disable_ack_=st->disable_ack;observed_=st->observed;rollback_ok_=st->rollback_ok;generation_=std::max(generation_,st->version);}
 const char*ro=getenv("TIPE_LEARNER_READ_ONLY"); bool readonly=(ro&&std::string(ro)=="1")||mode==LearnerMode::Control;
 writer_fd_=::open((dir_+"/writer.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
 if(writer_fd_>=0&&!readonly&&::flock(writer_fd_,LOCK_EX|LOCK_NB)==0){writer_=true;remove_stale_temps(dir_);if(cloud_options.enabled&&!cloud_options.endpoint.empty()&&cloud_consent){cloud_worker_=std::make_unique<CloudWorker>(dir_+"/cloud.state");cloud_worker_->configure(std::move(cloud_options),true);cloud_worker_->pause(c.paused || !c.enabled || !model_settings_->cloud_enabled());}worker_=std::thread(&Learner::run,this);}else{if(writer_fd_>=0){::flock(writer_fd_,LOCK_UN);::close(writer_fd_);}writer_fd_=-1;refresh_observer();}
}
Learner::~Learner(){stop_=true;cv_.notify_all();if(worker_.joinable())worker_.join();if(writer_fd_>=0){::flock(writer_fd_,LOCK_UN);::close(writer_fd_);}}
Learner::Control Learner::read_control()const{Control c;std::error_code se;if(fs::exists(dir_+"/control.current",se)&&fs::file_size(dir_+"/control.current",se)>4096)return c;std::ifstream f(dir_+"/control.current");std::string line;if(!f||!std::getline(f,line)||line.size()>4096)return c;std::vector<std::string>a;size_t p=0,n;while((n=line.find('\t',p))!=std::string::npos){a.push_back(line.substr(p,n-p));p=n+1;}a.push_back(line.substr(p));try{if(a.size()>=7&&a[0]=="#tipe-control-v1"){c.revision=std::stoull(a[1]);c.paused=std::stoull(a[2])!=0;c.enabled=std::stoull(a[3])!=0;c.clear_revision=std::stoull(a[4]);c.rollback_revision=std::stoull(a[5]);c.disable_revision=std::stoull(a[6]);c.valid=true;}}catch(...){ }return c;}
std::optional<Learner::SharedState> Learner::read_state()const{std::error_code se;if(fs::exists(dir_+"/state.current",se)&&fs::file_size(dir_+"/state.current",se)>16384)return{};std::ifstream f(dir_+"/state.current");std::string l;if(!f||!std::getline(f,l)||l.size()>16384)return{};std::vector<std::string>a;size_t p=0,n;while((n=l.find('\t',p))!=std::string::npos){a.push_back(l.substr(p,n-p));p=n+1;}a.push_back(l.substr(p));try{if(a.size()<10||a[0]!="#tipe-state-v3")return{};SharedState s;s.ack=std::stoull(a[1]);s.clear_ack=std::stoull(a[2]);s.rollback_ack=std::stoull(a[3]);s.disable_ack=std::stoull(a[4]);s.version=std::stoull(a[5]);s.observed=std::stoull(a[6]);s.learned=std::stoull(a[7]);s.paused=std::stoull(a[8]);s.enabled=std::stoull(a[9]);s.rollback_ok=a.size()>10&&std::stoull(a[10]);s.status=a.size()>11?a[11]:"";return s;}catch(...){return{};}}
void Learner::push(Event e){if(e.sensitive||e.pinyin.size()>1024||e.committed.size()>1024||e.context.size()>1024||e.preedit.size()>4096||e.before.size()>4096||e.corrected_from.size()>64)return;if(!enabled_.load(std::memory_order_relaxed)||paused_.load(std::memory_order_relaxed))return;if(!e.ts_ms)e.ts_ms=nowms();if(cloud_worker_ && model_settings_->cloud_enabled()){cloud_worker_->pause(false);cloud_worker_->enqueue(e);}std::lock_guard l(mu_);if(resetting_||!enabled_||paused_)return;if(q_.size()>=2048){q_.pop_front();dropped_++;}q_.push_back(std::move(e));observed_++;cv_.notify_one();}
bool Learner::pause(bool p){return command(Command::Pause,p);}
bool Learner::set_enabled(bool on){return command(Command::Enable,on);}
bool Learner::paused()const{if(auto s=read_state())return s->paused;return paused_.load();}
bool Learner::enabled()const{if(auto s=read_state())return s->enabled;return enabled_.load();}
size_t Learner::observed()const{
 return status_snapshot().observed;
}
size_t Learner::learned()const{return status_snapshot().learned;}
size_t Learner::queue_depth()const{std::lock_guard l(mu_);return q_.size();}size_t Learner::dropped_events()const{return dropped_.load();}size_t Learner::publish_count()const{return publishes_.load();}uint64_t Learner::worker_cpu_us()const{return worker_cpu_us_.load(std::memory_order_acquire);}uint64_t Learner::worker_throttled_us()const{return worker_throttled_us_.load(std::memory_order_acquire);}uint64_t Learner::disk_bytes()const{refresh_disk_bytes();return disk_bytes_.load(std::memory_order_acquire);}
void Learner::refresh_disk_bytes() const {
  std::error_code ec;
  uint64_t total = 0;
  for (const auto &entry : fs::directory_iterator(dir_, ec)) {
    if (ec) break;
    std::error_code item_ec;
    if (!entry.is_regular_file(item_ec)) continue;
    const auto size = entry.file_size(item_ec);
    if (item_ec) continue;
    if (std::numeric_limits<uint64_t>::max() - total < size) {
      total = std::numeric_limits<uint64_t>::max();
      break;
    }
    total += static_cast<uint64_t>(size);
  }
  disk_bytes_.store(total, std::memory_order_release);
}
static std::string compact_pinyin(std::string s) {
  s.erase(std::remove(s.begin(), s.end(), '\''), s.end());
  return s;
}
int Learner::preference(const std::string&t)const{auto m=snapshot_.load();if(!m)return 0;auto i=m->text_preferences.find(t);return i==m->text_preferences.end()?0:i->second;}
std::vector<PersonalEntry> Learner::exact_entries(const std::string& raw,
                                                   const std::string& context) const {
  std::vector<PersonalEntry> out;
  auto m = snapshot_.load(); if (!m) return out;
  std::unordered_set<std::string> seen;
  const auto key = compact_pinyin(raw);
  auto add = [&](const std::string &ctx) {
    auto it = m->exact_index.find(key + "\t" + ctx);
    if (it == m->exact_index.end()) return;
    for (auto idx : it->second) {
      if (idx >= m->entries.size()) continue;
      const auto &e = m->entries[idx];
      if (e.count < 3 || !seen.insert(e.text).second) continue;
      out.push_back(e); if (out.size() >= 16) return;
    }
  };
  add(context); if (out.size() < 16 && !context.empty()) add("");
  return out;
}
bool Learner::literal_preferred(const std::string& raw, const std::string& context) const {
  if (raw.size() < 3 || raw.size() > 64) return false;
  auto snapshot = snapshot_.load();
  if (!snapshot) return false;
  auto found = snapshot->exact_index.find("@literal:" + raw + "\t" + context);
  if (found == snapshot->exact_index.end()) return false;
  for (const auto index : found->second)
    if (index < snapshot->entries.size() && snapshot->entries[index].count >= 3 &&
        snapshot->entries[index].text == raw) return true;
  return false;
}

std::vector<std::string> Learner::correction_candidates(const std::string& raw,
                                                        const std::string& context) const {
  auto m = snapshot_.load();
  if (!m) return {};
  auto it = m->corrections.find(context);
  if (it == m->corrections.end()) return {};
  return it->second.generate(raw, true);
}
std::shared_ptr<const LearningModel>Learner::model()const{return snapshot_.load();}
std::string Learner::status()const{if(auto s=read_state();s&&!s->status.empty())return s->status;std::lock_guard l(mu_);return status_;}
LearningStatus Learner::status_snapshot() const {
  LearningStatus out;
  const auto state = read_state();
  std::lock_guard l(mu_);
  auto model = snapshot_.load();
  const size_t local_learned = model ? model->entries.size() : 0;
  if (state) {
    // Read-only observers return one coherent durable generation.  A writer
    // also has newer in-memory events between state.current flushes, so retain
    // its local monotonic counter until the next state publication.
    if (writer_) {
      observed_ = std::max(observed_, state->observed);
      out.observed = observed_;
      out.learned = std::max(local_learned, state->learned);
    } else {
      out.observed = state->observed;
      out.learned = state->learned;
    }
    out.paused = state->paused;
    out.enabled = state->enabled;
    out.status = state->status;
    if (!writer_) observed_ = state->observed;
    clear_seen_ = state->clear_ack;
    clear_floor_.store(0, std::memory_order_release);
    out.ack = state->ack;
    out.clear_ack = state->clear_ack;
    out.rollback_ack = state->rollback_ack;
    out.disable_ack = state->disable_ack;
    out.version = state->version;
    out.rollback_ok = state->rollback_ok;
    out.valid = true;
    return out;
  }
  out.observed = observed_;
  out.learned = local_learned;
  out.paused = paused_;
  out.enabled = enabled_;
  out.status = status_;
  return out;
}
bool Learner::wait_until_idle(std::chrono::milliseconds t)const{std::unique_lock l(mu_);return idle_cv_.wait_for(l,t,[&]{return !busy_&&q_.empty();});}
CloudWorkerStatus Learner::cloud_status() const {
  if (cloud_worker_) return cloud_worker_->status();
  std::ifstream file(dir_ + "/cloud.state");
  std::string line;
  if (!file || !std::getline(file, line) || line.size() > 4096) return {};
  std::vector<std::string> fields; size_t begin = 0;
  while (true) { const size_t tab = line.find('\t', begin); if (tab == std::string::npos) { fields.push_back(line.substr(begin)); break; } fields.push_back(line.substr(begin, tab - begin)); begin = tab + 1; }
  if (fields.size() < 7 || fields[0] != "#tipe-cloud-v1") return {};
  CloudWorkerStatus result;
  if (fields[1] == "idle") result.state = CloudWorkerState::Idle;
  else if (fields[1] == "sending") result.state = CloudWorkerState::Sending;
  else if (fields[1] == "backoff") result.state = CloudWorkerState::Backoff;
  else if (fields[1] == "paused") result.state = CloudWorkerState::Paused;
  else if (fields[1] == "stopped") result.state = CloudWorkerState::Stopped;
  try { result.queued = std::stoull(fields[2]); result.sent = std::stoull(fields[3]); result.failed = std::stoull(fields[4]); result.retries = std::stoull(fields[5]); } catch (...) { return {}; }
  result.detail = fields[6];
  return result;
}
std::vector<CloudCandidate> Learner::cloud_candidates(std::string_view pinyin,
                                                      std::string_view context) const {
  if (cloud_worker_ && model_settings_->cloud_enabled() && enabled_.load() && !paused_.load())
    return cloud_worker_->candidates(pinyin, std::to_string(fnv(std::string(context))));
  return {};
}
void Learner::process(const Event& incoming) {
  // Repeated explicit raw-text submissions are evidence about this spelling,
  // never a heuristic that suppresses similar pinyin in other applications.
  Event literal;
  const bool raw_commit = incoming.type == EventType::Commit && incoming.pinyin.empty() &&
      (incoming.key == "Enter" || incoming.key == "Space") &&
      incoming.committed.size() >= 3 && incoming.committed.size() <= 64 &&
      std::all_of(incoming.committed.begin(),incoming.committed.end(),[](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
      });
  if (raw_commit) {
    literal = incoming;
    literal.pinyin = "@literal:" + incoming.committed;
    literal.before.clear(); literal.remaining.clear(); literal.corrected_from.clear();
  }
  const Event &e = raw_commit ? literal : incoming;
  if ((e.type == EventType::KeyDown &&
       (e.key == "BackSpace" || e.key == "Delete" || e.key == "Esc")) ||
      e.type == EventType::ModeSwitch) {
    pending_.erase(e.session);
    return;
  }
  if ((e.type != EventType::CandidateSelect && e.type != EventType::Commit) ||
      e.committed.empty() || e.pinyin.empty() || e.committed.size() > 512 ||
      e.pinyin.size() > 512) return;
  if (e.retained && *e.retained != e.committed) { pending_.erase(e.session); return; }
  std::string spelling;
  if (e.before.size() >= e.remaining.size() && e.before.ends_with(e.remaining))
    spelling = e.before.substr(0, e.before.size() - e.remaining.size());
  if (pending_.size() >= 1024 && !pending_.contains(e.session))
    pending_.erase(pending_.begin());
  auto &chain = pending_[e.session];
  // Do not concatenate unrelated contexts or stale/incompatible suffixes.
  if (!chain.entry.text.empty() &&
      (chain.entry.context != e.context ||
       (e.ts_ms > chain.touched && e.ts_ms - chain.touched > 30000) ||
       (!e.before.empty() && !e.before.starts_with(chain.remaining)))) chain = {};
  if (!chain.entry.pinyin.empty()) chain.entry.pinyin += "'";
  chain.entry.pinyin += e.pinyin;
  chain.entry.text += e.committed;
  chain.entry.context = e.context;
  chain.raw += spelling;
  chain.inferred = chain.inferred || e.inferred;
  if (chain.inferred) chain.corrected_from.clear();
  else if (chain.corrected_from.empty() && !e.corrected_from.empty())
    chain.corrected_from = e.corrected_from;
  chain.remaining = e.remaining;
  chain.touched = e.ts_ms;
  if (chain.entry.pinyin.size() > 512 || chain.entry.text.size() > 512 || chain.raw.size() > 512) {
    pending_.erase(e.session); return;
  }
  if (!e.remaining.empty()) return;
  auto phrase = std::move(chain.entry);
  auto typed = std::move(chain.raw);
  auto corrected_from = std::move(chain.corrected_from);
  const bool inferred = chain.inferred;
  pending_.erase(e.session);
  // A generated correction is not new independent evidence. Otherwise its
  // acceptance repeatedly trains both exact preferences and the language
  // model on their own predictions.
  if (inferred) return;
  auto record = [&](const std::string& pinyin) {
    const auto key = pinyin + "\t" + phrase.text + "\t" + phrase.context;
    if (counts_.size() >= 8192 && !counts_.contains(key)) {
      auto least = std::min_element(counts_.begin(), counts_.end(),
          [](const auto& a, const auto& b) { return a.second.count < b.second.count; });
      if (least != counts_.end()) counts_.erase(least);
    }
    auto &entry = counts_[key];
    entry.pinyin = pinyin; entry.text = phrase.text; entry.context = phrase.context;
    if (entry.count < 1000000) ++entry.count;
  };
  record(phrase.pinyin);
  if(!phrase.pinyin.starts_with("@literal:") && phrase.text!=typed) {
    for(const auto &spelling:{compact_pinyin(phrase.pinyin),typed})
      if(!spelling.empty()) counts_.erase("@literal:"+spelling+"\t"+spelling+"\t"+phrase.context);
  }
  // Store the observed abbreviation as its own exact spelling evidence.
  // It never generalizes to other inputs, and must pass the same repetition
  // threshold as the canonical phrase before it can affect candidates.
  if (!typed.empty() && compact_pinyin(typed) != compact_pinyin(phrase.pinyin)) record(typed);
  // Generalized single-edit evidence is deliberately separate from exact
  // preferences. It only becomes active after repeated, varied explicit
  // correction examples in the same low-sensitivity application context.
  if (!inferred && !corrected_from.empty() && phrase.context.size() <= 512 &&
      clean(phrase.context) == phrase.context) {
    const auto canonical = compact_pinyin(phrase.pinyin);
    auto it = corrections_.find(phrase.context);
    if (it != corrections_.end()) it->second.observe(corrected_from, canonical);
    else if (corrections_.size() < 16) {
      correction::RuleStore rules;
      if (rules.observe(corrected_from, canonical))
        corrections_.emplace(phrase.context, std::move(rules));
    }
  }
}
void Learner::publish(bool force,bool rotate){
 auto m=std::make_shared<LearningModel>();m->version=++generation_;
 for(auto const&[k,v]:counts_)m->entries.push_back(v);
 // Keep publication deterministic when counts tie.  `counts_` is an
 // unordered map, so comparing only count would otherwise rewrite the
 // model on every batch even when the learned data did not change.
 std::sort(m->entries.begin(),m->entries.end(),[](const auto&a,const auto&b){
   if (a.count != b.count) return a.count > b.count;
   if (a.pinyin != b.pinyin) return a.pinyin < b.pinyin;
   if (a.text != b.text) return a.text < b.text;
   return a.context < b.context;
 });
 if(m->entries.size()>4096)m->entries.resize(4096);
 for(size_t i=0;i<m->entries.size();++i){auto const&e=m->entries[i];m->text_preferences[e.text]=std::min(1000000,m->text_preferences[e.text]+(int)std::min<uint64_t>(e.count,1000000));m->exact_index[compact_pinyin(e.pinyin)+"\t"+e.context].push_back(i);}
 for(const auto &entry:m->entries)
   if(!entry.pinyin.starts_with("@literal:"))
     m->language_model.observe(entry.text,entry.context,entry.count);
 m->corrections = corrections_;
 std::string body;for(auto const&e:m->entries)body+=clean(e.pinyin)+"\t"+clean(e.text)+"\t"+std::to_string(e.count)+"\t"+clean(e.context)+"\n";
 std::vector<std::pair<std::string, correction::Rule>> correction_rows;
 for (const auto &[ctx, store] : corrections_)
   for (const auto &r : store.rules()) correction_rows.emplace_back(ctx, r);
 std::sort(correction_rows.begin(), correction_rows.end(),
           [](const auto &a, const auto &b) {
             if (a.first != b.first) return a.first < b.first;
             const auto &x = a.second; const auto &y = b.second;
             if (x.kind != y.kind) return x.kind < y.kind;
             if (x.typed != y.typed) return x.typed < y.typed;
             if (x.missing != y.missing) return x.missing < y.missing;
             if (x.left != y.left) return x.left < y.left;
             return x.right < y.right;
           });
 for (const auto &[ctx, r] : correction_rows) {
     body += "#correction\t" + clean(ctx) + "\t" + std::string(1, r.missing) +
             "\t" + std::string(1, r.left) + "\t" + std::string(1, r.right) +
             "\t" + std::to_string(r.support) + "\t";
     for (size_t i = 0; i < r.corrected_pinyin.size(); ++i) {
       if (i) body += ",";
       body += clean(r.corrected_pinyin[i]);
     }
     body += "\t";
     for (size_t i = 0; i < r.observed_raw.size(); ++i) {
       if (i) body += ",";
       body += clean(r.observed_raw[i]);
     }
     if (r.kind != correction::EditKind::Omission)
       body += "\t" + std::to_string(static_cast<int>(r.kind)) + "\t" +
               (r.typed ? std::string(1,r.typed) : "-");
     body += "\n";
   }

 if(!force&&current_valid_&&body==persisted_body_)return;
 auto cur=dir_+"/model.current",prev=dir_+"/model.previous",ct=cur+".tmp",pt=prev+".tmp";
 // Keep only current and previous generations.  The temporary file is
 // fsynced before each rename, so a crash leaves at least one valid model.
 {
   std::ofstream f(ct);if(!f)throw std::runtime_error("model temp");
   f<<"#tipe-v1\t"<<m->version<<"\t"<<fnv(body)<<"\n"<<body;f.close();if(!f)throw std::runtime_error("model write");::chmod(ct.c_str(),0600);crash_point("model-before-temp-fsync");fsync_file(ct);crash_point("model-after-temp-fsync");
 }
 std::error_code e;
 if(rotate&&current_valid_&&fs::exists(cur)){
   fs::copy_file(cur,pt,fs::copy_options::overwrite_existing,e);if(e)throw std::runtime_error("model backup");
   ::chmod(pt.c_str(),0600);crash_point("model-before-previous-fsync");fsync_file(pt);crash_point("model-after-previous-fsync");crash_point("model-before-previous-rename");fs::rename(pt,prev,e);if(e)throw std::runtime_error("model previous");crash_point("model-after-previous-rename");fsync_directory(dir_);crash_point("model-after-previous-dir-fsync");
 }
 crash_point("model-before-current-rename");fs::rename(ct,cur,e);if(e)throw std::runtime_error("model activate");crash_point("model-after-current-rename");fsync_file(cur);crash_point("model-after-current-fsync");fsync_directory(dir_);crash_point("model-after-current-dir-fsync");
 remove_legacy_generations(dir_);
 snapshot_.store(std::shared_ptr<const LearningModel>(m));persisted_body_=body;current_valid_=true;publishes_++;refresh_disk_bytes();
}
bool Learner::load_file(const std::string&path,bool install){
   std::error_code fe; if(fs::exists(path,fe)&&fs::is_regular_file(path,fe)&&fs::file_size(path,fe)>8*1024*1024)return false;
   std::ifstream f(path);if(!f)return false;std::string h;if(!std::getline(f,h))return false;
   std::vector<std::string>a;size_t p=0,n;while((n=h.find('\t',p))!=std::string::npos){a.push_back(h.substr(p,n-p));p=n+1;}a.push_back(h.substr(p));
   if(a.size()!=3||a[0]!="#tipe-v1")return false;std::string body((std::istreambuf_iterator<char>(f)),{});if(body.size()>8*1024*1024)return false;
   uint64_t ver,ck;try{ver=std::stoull(a[1]);ck=std::stoull(a[2]);}catch(...){return false;}if(fnv(body)!=ck)return false;
   std::istringstream rows(body);std::string line;size_t nrow=0;std::unordered_map<std::string,PersonalEntry> parsed;std::unordered_map<std::string,correction::RuleStore> parsed_corrections;
   auto split_csv = [](const std::string &s) {
     std::vector<std::string> v; size_t b=0,p;
     while ((p=s.find(',',b))!=std::string::npos) {
       if (p==b) return std::vector<std::string>{};
       v.push_back(s.substr(b,p-b)); b=p+1;
     }
     if (b<s.size()) v.push_back(s.substr(b));
     return v;
   };
   auto valid_ascii = [](const std::string &s, size_t max) {
     return !s.empty() && s.size()<=max && std::all_of(s.begin(),s.end(),[](unsigned char c){return c>='a'&&c<='z';});
   };
   while(std::getline(rows,line)){if(++nrow>4096+16*correction::RuleStore::kMaxRules)return false;std::vector<std::string>x;size_t q=0,z;while((z=line.find('\t',q))!=std::string::npos){x.push_back(line.substr(q,z-q));q=z+1;}x.push_back(line.substr(q));
     if (!x.empty() && x[0] == "#correction") {
       if ((x.size() != 8 && x.size() != 10) || x[1].size() > 512 || x[2].size()!=1 || x[3].size()!=1 || x[4].size()!=1) return false;
       correction::Rule rule; rule.missing=x[2][0]; rule.left=x[3][0]; rule.right=x[4][0];
       try { rule.support=std::stoull(x[5]); } catch (...) { return false; }
       if (x.size() == 10) {
         if (x[8].size() != 1 || x[8][0] < '1' || x[8][0] > '3' || x[9].size() != 1)
           return false;
         rule.kind = static_cast<correction::EditKind>(x[8][0]-'0');
         rule.typed = x[9] == "-" ? 0 : x[9][0];
       }
       if (!correction::RuleStore::valid_rule(rule)) return false;
       auto malformed_csv = [](const std::string &s) {
         return s.empty() ? false : s.front() == ',' || s.back() == ',' || s.find(",,") != std::string::npos;
       };
       if (malformed_csv(x[6]) || malformed_csv(x[7])) return false;
       rule.corrected_pinyin=split_csv(x[6]); rule.observed_raw=split_csv(x[7]);
       if (rule.corrected_pinyin.size()>correction::RuleStore::kMaxVariantsPerRule ||
           rule.observed_raw.size()>correction::RuleStore::kMaxVariantsPerRule ||
           rule.corrected_pinyin.empty()) return false;
       for (const auto &v : rule.corrected_pinyin)
         if (!valid_ascii(v, correction::RuleStore::kMaxRawLength + 1)) return false;
       for (const auto &v : rule.observed_raw)
         if (!valid_ascii(v, correction::RuleStore::kMaxRawLength)) return false;
       auto it = parsed_corrections.find(x[1]);
       if (it == parsed_corrections.end()) {
         if (parsed_corrections.size() >= 16) return false;
         it = parsed_corrections.emplace(x[1], correction::RuleStore{}).first;
       }
       const auto before = it->second.size();
       it->second.absorb(rule);
       if (it->second.size() == before) return false;
       continue;
     }
     if(x.size()!=4||x[0].size()>512||x[1].size()>512||x[3].size()>512)return false;uint64_t c;try{c=std::stoull(x[2]);}catch(...){return false;}if(c>1000000)return false;PersonalEntry e{x[0],x[1],x[3],c};parsed[x[0]+"\t"+x[1]+"\t"+x[3]]=e;if(parsed.size()>4096)return false;}
   if(!install)return true;
   corrections_=std::move(parsed_corrections);
   counts_=std::move(parsed);generation_=ver;persisted_body_=body;current_valid_=(path==dir_+"/model.current");auto m=std::make_shared<LearningModel>();m->version=ver;for(auto const&[k,v]:counts_)m->entries.push_back(v);for(size_t i=0;i<m->entries.size();++i){auto const&e=m->entries[i];m->text_preferences[e.text]+=std::min(1000000,(int)e.count);m->exact_index[compact_pinyin(e.pinyin)+"\t"+e.context].push_back(i);}auto training=m->entries;std::sort(training.begin(),training.end(),[](const auto&a,const auto&b){if(a.count!=b.count)return a.count>b.count;if(a.pinyin!=b.pinyin)return a.pinyin<b.pinyin;if(a.text!=b.text)return a.text<b.text;return a.context<b.context;});for(const auto &entry:training)if(!entry.pinyin.starts_with("@literal:"))m->language_model.observe(entry.text,entry.context,entry.count);m->corrections=corrections_;snapshot_.store(std::shared_ptr<const LearningModel>(m));return true;
}
void Learner::load(){
 if(load_file(dir_+"/model.current"))return;
   counts_.clear(); corrections_.clear(); current_valid_=false;auto empty=std::make_shared<LearningModel>();snapshot_.store(std::shared_ptr<const LearningModel>(empty));load_file(dir_+"/model.previous");
}
void Learner::write_state(){Lock lk(dir_+"/state.lock",LOCK_EX);if(!lk){state_dirty_=true;return;}size_t o,l;bool p,en;std::string st;{std::lock_guard g(mu_);o=observed_;p=paused_;en=enabled_;st=status_;}auto m=snapshot_.load();l=m?m->entries.size():0;std::string path=dir_+"/state.current",tmp=path+".tmp";std::ofstream f(tmp);if(!f){state_dirty_=true;return;}f<<"#tipe-state-v3\t"<<ack_.load()<<"\t"<<clear_ack_<<"\t"<<rollback_ack_<<"\t"<<disable_ack_<<"\t"<<generation_<<"\t"<<o<<"\t"<<l<<"\t"<<(p?1:0)<<"\t"<<(en?1:0)<<"\t"<<(rollback_ok_?1:0)<<"\t"<<clean(st)<<"\n";f.close();if(!f){state_dirty_=true;return;}::chmod(tmp.c_str(),0600);crash_point("state-before-temp-fsync");fsync_file(tmp);crash_point("state-after-temp-fsync");std::error_code e;crash_point("state-before-rename");fs::rename(tmp,path,e);crash_point("state-after-rename");if(!e)fsync_directory(dir_);crash_point("state-after-dir-fsync");state_dirty_=static_cast<bool>(e);}
void Learner::apply_control(const Control&c){
 bool changed=false;
 if(c.revision>gates_revision_){
   paused_=c.paused; enabled_=c.enabled; gates_revision_=c.revision;
   if (cloud_worker_) cloud_worker_->pause(c.paused || !c.enabled);
   if (c.enabled && !c.paused && c.clear_revision <= clear_ack_ &&
       c.rollback_revision <= rollback_ack_ && c.disable_revision <= disable_ack_)
     cancel_batch_.store(false, std::memory_order_release);
   requested_revision_=c.revision; changed=true;
 }
 if(!c.enabled && c.disable_revision>disable_ack_){
   std::lock_guard l(mu_); q_.clear(); disable_ack_=c.disable_revision; changed=true;
 }
 if(c.clear_revision>clear_ack_){
   if (cloud_worker_) cloud_worker_->cancel();
   {
     std::lock_guard l(mu_); resetting_=true; q_.clear(); observed_=0; busy_=true;
   }
   try {
  std::lock_guard d(request_mu_);
     counts_.clear(); pending_.clear(); corrections_.clear();
     generation_=std::max(generation_,c.clear_revision);
     persisted_body_.clear(); current_valid_=false;
     // Publish the empty generation before deleting the previous generation.
     // If writing fails, the old current model is still intact and can be
     // retried; removing both files first made a crash leave no valid model.
     publish(true,false);
     std::error_code e;
     fs::remove(dir_+"/model.previous",e);
     fs::remove(dir_+"/model.previous.tmp",e);
     fs::remove(dir_+"/model.current.tmp",e);
     fsync_directory(dir_);
     clear_ack_=c.clear_revision;
     {
       std::lock_guard l(mu_); resetting_=false; busy_=false; status_="等待输入";
     }
   } catch (...) {
     std::lock_guard l(mu_); resetting_=false; busy_=false;
     throw;
   }
   changed=true;
 }
 if(c.rollback_revision>rollback_ack_){
   if (cloud_worker_) cloud_worker_->cancel();
   bool ok=false;
   std::lock_guard d(request_mu_);
   auto prev=dir_+"/model.previous",cur=dir_+"/model.current",tmp=cur+".tmp";
   std::error_code e;
   if(load_file(prev,false)&&fs::copy_file(prev,tmp,fs::copy_options::overwrite_existing,e)){
     ::chmod(tmp.c_str(),0600); fsync_file(tmp); fs::rename(tmp,cur,e);
     if(!e){
       fsync_directory(dir_); auto next_generation=generation_;
       counts_.clear(); corrections_.clear(); pending_.clear(); current_valid_=false; load();
       generation_=std::max(generation_,next_generation); ok=true;
     }
   }
   rollback_ok_=ok; rollback_ack_=c.rollback_revision; changed=true;
 }
 if (c.enabled && !c.paused && c.clear_revision <= clear_ack_ &&
     c.rollback_revision <= rollback_ack_ && c.disable_revision <= disable_ack_)
   cancel_batch_.store(false, std::memory_order_release);
 if(changed){ack_=std::max(ack_.load(),c.revision);state_dirty_=true;write_state();}
}
// nice(10) changes priority but does not cap consumption when the machine is
// otherwise idle. After each complete transaction, yield one microsecond for
// each microsecond of writer CPU, including model sorting and publication.
// Thread CPU deliberately excludes fsync waits and test-delay sleeps, so slow
// I/O is not charged twice. This adds no work to push() or candidate lookup.
void Learner::throttle_worker(uint64_t batch_cpu_us) {
  worker_cpu_us_.fetch_add(batch_cpu_us, std::memory_order_relaxed);
  const auto cooldown = std::chrono::microseconds(std::max<uint64_t>(5000, batch_cpu_us));
  const auto deadline = std::chrono::steady_clock::now() + cooldown;
  const auto started = std::chrono::steady_clock::now();
  while (!stop_.load(std::memory_order_acquire) &&
         !cancel_batch_.load(std::memory_order_acquire)) {
    auto lock = std::unique_lock(mu_);
    const auto wake = std::min(deadline, std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(50));
    cv_.wait_until(lock, wake, [&] {
      return stop_.load(std::memory_order_acquire) ||
             cancel_batch_.load(std::memory_order_acquire);
    });
    lock.unlock();
    if (std::chrono::steady_clock::now() >= deadline) break;
    // A control instance in another process cannot notify cv_. Poll its small
    // bounded file between slices so pause/clear/rollback remain responsive.
    const auto control = read_control();
    if (control.revision > gates_revision_) break;
  }
  worker_throttled_us_.fetch_add(static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started).count()),
      std::memory_order_relaxed);
}
void Learner::run(){ (void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10); retry_at_=std::chrono::steady_clock::now(); auto next_model_refresh = retry_at_; while(!stop_){Control c=read_control();try{if (std::chrono::steady_clock::now() >= next_model_refresh) { model_settings_->refresh();
      if(cloud_worker_ && !std::getenv("TIPE_CLOUD_API_KEY")) {
        const auto path=model_settings_->cloud_api_key_path();
        if(path!=cloud_options_.api_key_path) {
          cloud_options_.api_key_path=path;cloud_options_.include_api_key=!path.empty();
          cloud_worker_->configure(cloud_options_,true);
        }
      }
      next_model_refresh = std::chrono::steady_clock::now() + std::chrono::seconds(1); } apply_control(c); if (cloud_worker_) cloud_worker_->pause(c.paused || !c.enabled || !model_settings_->cloud_enabled());}catch(...){ { std::lock_guard l(mu_);status_="下次自动重试";retry_at_=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);busy_=false; } write_state();std::this_thread::sleep_for(std::chrono::milliseconds(100));continue;}std::unique_lock l(mu_);cv_.wait_for(l,std::chrono::milliseconds(50),[&]{return stop_||(!q_.empty()&&!paused_&&!resetting_&&enabled_&&std::chrono::steady_clock::now()>=retry_at_);});if(stop_)break;if(paused_||!enabled_||resetting_)continue;if(q_.empty())continue;l.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(40));l.lock();if(q_.empty()||paused_||!enabled_||resetting_)continue;std::deque<Event> batch;batch.swap(q_);busy_=true;status_="后台学习中";l.unlock();
 coalesce_batch(batch);
 // Serialize mutable training state with clear/rollback control operations.
 // Input events remain protected by mu_; this lock covers process(), snapshots
 // and publication so control cannot race a batch midway through updating it.
 const auto work_cpu_started = thread_cpu_us();
 std::unique_lock req(request_mu_);
 auto counts_before=counts_;auto pending_before=pending_;auto corrections_before=corrections_;auto gen_before=generation_; size_t total_before=0; for(auto const&kv:counts_before) total_before += kv.second.count; const char *delay_env=std::getenv("TIPE_LEARNER_TEST_BATCH_DELAY_US"); const long test_delay=delay_env?std::max(0L,std::min(100000L,std::strtol(delay_env,nullptr,10))):0; bool failed=false, cancelled=false, discard=false;try{for(size_t i=0;i<batch.size();++i){if(i % 16 == 0){ const auto c=read_control(); const bool destructive=c.clear_revision>clear_ack_ || c.rollback_revision>rollback_ack_ || c.disable_revision>disable_ack_; if(c.revision>gates_revision_ && (c.paused || !c.enabled || destructive)){cancel_batch_.store(true, std::memory_order_release); discard=destructive;} else if(c.revision>gates_revision_ && c.enabled && !c.paused){cancel_batch_.store(false, std::memory_order_release);} } if(cancel_batch_.load(std::memory_order_acquire)){cancelled=true;break;} process(batch[i]); if(test_delay) std::this_thread::sleep_for(std::chrono::microseconds(test_delay));} if(cancelled){counts_=std::move(counts_before);pending_=std::move(pending_before);corrections_=std::move(corrections_before);generation_=gen_before;std::lock_guard g(mu_);if(discard){dropped_ += batch.size() + q_.size(); batch.clear(); q_.clear();} else {while(!batch.empty()&&q_.size()<2048){q_.push_front(std::move(batch.back()));batch.pop_back();}dropped_ += batch.size(); batch.clear();}status_=discard?"已清空":"已暂停";} else {size_t total_after=0; for(auto const&kv:counts_) total_after += kv.second.count; if(total_after!=total_before || correction_support(corrections_)!=correction_support(corrections_before)) publish();}}catch(...){failed=true;counts_=std::move(counts_before);pending_=std::move(pending_before);corrections_=std::move(corrections_before);generation_=gen_before;std::lock_guard g(mu_);while(!batch.empty()&&q_.size()<2048){q_.push_front(std::move(batch.back()));batch.pop_back();}dropped_ += batch.size(); batch.clear();retry_at_=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);status_="下次自动重试";cv_.notify_one();}if(!failed&&!cancelled){retry_at_=std::chrono::steady_clock::now();std::lock_guard g(mu_);status_="已学习";}write_state(); const auto work_cpu_end=thread_cpu_us(); const auto work_cpu=work_cpu_end>=work_cpu_started?work_cpu_end-work_cpu_started:0; req.unlock(); throttle_worker(work_cpu); l.lock();busy_=false;l.unlock();idle_cv_.notify_all();}}
bool Learner::command(Command cmd,bool value){
 Control c;std::string path=dir_+"/control.current",tmp=path+".tmp";{Lock lk(dir_+"/control.lock",LOCK_EX);if(!lk)return false;c=read_control();if(fs::exists(path) && !c.valid)return false;c.revision++;switch(cmd){case Command::Pause:c.paused=value;break;case Command::Enable:c.enabled=value;if(!value)c.disable_revision=c.revision;break;case Command::Clear:c.clear_revision=c.revision;break;case Command::Rollback:c.rollback_revision=c.revision;break;}std::ofstream f(tmp);if(!f)return false;f<<"#tipe-control-v1\t"<<c.revision<<"\t"<<(c.paused?1:0)<<"\t"<<(c.enabled?1:0)<<"\t"<<c.clear_revision<<"\t"<<c.rollback_revision<<"\t"<<c.disable_revision<<"\n";f.close();if(!f)return false;::chmod(tmp.c_str(),0600);fsync_file(tmp);std::error_code e;fs::rename(tmp,path,e);if(e)return false;fsync_directory(dir_);}
 if ((cmd == Command::Pause && value) || (cmd == Command::Enable && !value) || cmd == Command::Clear || cmd == Command::Rollback)
   cancel_batch_.store(true, std::memory_order_release);
 if (cloud_worker_ && ((cmd == Command::Pause && value) || (cmd == Command::Enable && !value)))
   cloud_worker_->pause(true);
 if (cloud_worker_ && (cmd == Command::Clear || cmd == Command::Rollback))
   cloud_worker_->cancel();
 // Reflect a successful clear immediately in a read-only control instance.
 // The atomic snapshot is safe to replace while the writer rolls its batch
 // back; the durable clear is performed by apply_control after cancellation.
 if(cmd==Command::Clear){
   clear_floor_.store(c.clear_revision,std::memory_order_release);
   std::lock_guard l(mu_); q_.clear();
 }
 cv_.notify_one();if(!writer_){try_temporary_writer();}auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(1800);for(;std::chrono::steady_clock::now()<until;){if(auto st=read_state()){if(st->ack>=c.revision){paused_=st->paused;enabled_=st->enabled;if(cmd==Command::Clear){auto empty=std::make_shared<LearningModel>();snapshot_.store(std::shared_ptr<const LearningModel>(empty));std::lock_guard l(mu_);observed_=0;}if(cmd==Command::Rollback)return st->rollback_ack>=c.revision&&st->rollback_ok;return true;}}if(!writer_)try_temporary_writer();std::this_thread::sleep_for(std::chrono::milliseconds(20));}return false;}
bool Learner::try_temporary_writer(){if(writer_)return false;Lock lk(dir_+"/writer.lock");if(!lk)return false;Control c=read_control();try{load();apply_control(c);}catch(...){return false;}refresh_observer();return true;}
void Learner::refresh_observer(){if(auto s=read_state()){std::lock_guard l(mu_);observed_=s->observed;paused_=s->paused;enabled_=s->enabled;status_=s->status;}}
bool Learner::export_data(const std::string&p,std::stop_token cancellation)const{
 auto m=snapshot_.load(); if(!m || cancellation.stop_requested())return false;
 // Export is user data: write it with restrictive permissions from the first
 // byte and publish atomically, so a reader cannot observe a partial file.
 std::string tmp=p+".tmp.XXXXXX";
 int fd=::mkostemp(tmp.data(),O_CLOEXEC);
 if(fd<0)return false;
 struct TemporaryExport {
   int fd; const std::string &path;
   ~TemporaryExport(){if(fd>=0)::close(fd);::unlink(path.c_str());}
 } temporary{fd,tmp};
 if(::fchmod(fd,0600)!=0)return false;
 std::string body;
 for(auto const&e:m->entries) {
   if(cancellation.stop_requested())return false;
   body+=clean(e.pinyin)+"\t"+clean(e.text)+"\t"+std::to_string(e.count)+"\t"+clean(e.context)+"\n";
 }
 std::vector<std::pair<std::string, correction::Rule>> correction_rows;
 for (const auto &[ctx, store] : m->corrections)
   for (const auto &r : store.rules()) correction_rows.emplace_back(ctx, r);
 std::sort(correction_rows.begin(), correction_rows.end(),
           [](const auto &a, const auto &b) {
             if (a.first != b.first) return a.first < b.first;
             const auto &x = a.second; const auto &y = b.second;
             if (x.kind != y.kind) return x.kind < y.kind;
             if (x.typed != y.typed) return x.typed < y.typed;
             if (x.missing != y.missing) return x.missing < y.missing;
             if (x.left != y.left) return x.left < y.left;
             return x.right < y.right;
           });
 for (const auto &[ctx, r] : correction_rows) {
   if(cancellation.stop_requested())return false;
   body += "#correction\t" + clean(ctx) + "\t" + std::string(1, r.missing) +
           "\t" + std::string(1, r.left) + "\t" + std::string(1, r.right) +
           "\t" + std::to_string(r.support) + "\t";
   for (size_t i = 0; i < r.corrected_pinyin.size(); ++i) {
     if (i) body += ",";
     body += clean(r.corrected_pinyin[i]);
   }
   body += "\t";
   for (size_t i = 0; i < r.observed_raw.size(); ++i) {
     if (i) body += ",";
     body += clean(r.observed_raw[i]);
   }
   if (r.kind != correction::EditKind::Omission)
     body += "\t" + std::to_string(static_cast<int>(r.kind)) + "\t" +
             (r.typed ? std::string(1,r.typed) : "-");
   body += "\n";
 }
 const char* data=body.data(); size_t left=body.size();
 while(left){
   if(cancellation.stop_requested())return false;
   ssize_t n=::write(fd,data,std::min<size_t>(left,65536));
   if(n<0 && errno==EINTR)continue;
   if(n<=0)return false;
   data+=n;left-=static_cast<size_t>(n);
 }
 if(cancellation.stop_requested() || ::fsync(fd)!=0)return false;
 ::close(fd);temporary.fd=-1;
 // Rename is the commit point. A cancelled export preserves any previous
 // destination; each invocation owns a unique temporary even across processes.
 if(cancellation.stop_requested())return false;
 std::error_code e; fs::rename(tmp,p,e); if(e)return false;
 fsync_directory(fs::path(p).parent_path().string().empty()?".":fs::path(p).parent_path().string());
 return true;
}
bool Learner::clear(){return command(Command::Clear);}bool Learner::rollback(){return command(Command::Rollback);}
}
