#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <stop_token>
#include <unordered_map>
#include <vector>
#include <string_view>
#include "tipe/correction.hpp"
#include "tipe/local_language_model.hpp"
namespace tipe {
enum class EventType { KeyDown, KeyUp, CandidateOpen, CandidateMove, CandidateSelect, Commit, ModeSwitch, CandidateClose };
struct Event { EventType type{}; std::string key, preedit, committed, context; uint64_t ts_ms{}; std::string pinyin; uint64_t session{}; bool sensitive{}; std::string remaining, before, corrected_from; std::optional<std::string> retained; uint32_t interval_ms{}, delete_interval_ms{}; bool inferred{}; };
// A deliberately small, opt-in payload contract for the cloud enhancer.
// It contains only semantic commit data; the transport is separately gated
// and remains disabled by default.
// Raw key names, preedit text, surrounding text, retained text and session
// identifiers are intentionally absent so a caller cannot accidentally send
// the complete keyboard stream.
struct CloudPayload {
  std::string pinyin;
  std::string committed;
  std::string context_token;
  uint32_t interval_ms{};
  uint32_t delete_interval_ms{};
  uint64_t request_id{};
};

// Return a minimal payload only after explicit user consent.  Sensitive
// events, non semantic key events and malformed/oversized values are always
// rejected.  The context is represented by a stable one-way token.
std::optional<CloudPayload> make_cloud_payload(const Event &, bool consent,
                                                uint64_t request_id = 0);

// Long-running cloud work is cancellable. This token is cheap to poll from a
// worker and has no background thread of its own.
class CloudCancellation {
 public:
  void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
  void reset() noexcept { cancelled_.store(false, std::memory_order_release); }
  bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }
 private:
  std::atomic<bool> cancelled_{false};
};

// A deliberately small HTTP transport for the opt-in cloud enhancer.  The
// transport is disabled unless both `enabled` and the per-request consent
// argument are true.  It accepts only the semantic CloudPayload produced by
// make_cloud_payload; key names, preedit, surrounding text and session data
// have no representation at this layer. Loopback HTTP is supported for local
// services/tests. HTTPS uses OpenSSL with TLS 1.2+, CA and hostname/IP
// verification. Remote clear-text HTTP is rejected. API keys are read only
// from an owner-only regular file and sent as an Authorization bearer token.
struct CloudTransportOptions {
  std::string endpoint; // https://host:port/path (or loopback http for tests)
  bool enabled{};
  // API key use is separately opt-in. The file is read only after
  // send_cloud_request receives per-request consent; it is never logged.
  bool include_api_key{};
  std::string api_key_path;
  std::string ca_certificate_path; // empty uses the system trust store
  std::chrono::milliseconds timeout{1500};
  size_t max_response_bytes{64 * 1024};
};

enum class CloudTransportStatus {
  Sent,
  Disabled,
  ConsentRequired,
  InvalidPayload,
  Cancelled,
  TimedOut,
  TransportError,
  ProtocolError,
};

struct CloudTransportResult {
  CloudTransportStatus status{CloudTransportStatus::TransportError};
  int http_status{};
  uint64_t request_id{};
  std::string response;
  std::string detail;
  bool ok() const noexcept { return status == CloudTransportStatus::Sent; }
};

// A response candidate is deliberately separate from the local decoder and
// personal model. It is ephemeral, context-scoped data from an explicitly
// enabled cloud request.
struct CloudCandidate {
  std::string pinyin;
  std::string text;
  int score{};
};

// Parse the bounded cloud response schema. The parser accepts an object with
// a `candidates` array of {pinyin,text,score} objects and an optional
// request_id matching expected_request_id. Unknown fields are rejected inside
// candidate objects, malformed UTF-8 and duplicate candidates are rejected,
// and all sizes/counts are bounded. An object without `candidates` is a valid
// acknowledgement and returns an empty vector. No network or model state is
// touched by this function.
bool parse_cloud_candidates(std::string_view response, uint64_t expected_request_id,
                            std::vector<CloudCandidate> &out,
                            std::string *detail = nullptr);

// Short-lived in-memory cache for validated cloud candidates. It is bounded,
// context-token keyed, and never persisted or merged into the TiP model.
class CloudCandidateCache {
 public:
  explicit CloudCandidateCache(std::chrono::milliseconds ttl = std::chrono::seconds(30),
                               size_t max_entries = 128);
  bool put(uint64_t request_id, std::string context_token,
           std::vector<CloudCandidate> candidates);
  std::vector<CloudCandidate> lookup(std::string_view pinyin,
                                     std::string_view context_token) const;
  void clear();
  size_t size() const;
 private:
  struct Entry { uint64_t request_id{}; std::string pinyin, context; std::vector<CloudCandidate> candidates; std::chrono::steady_clock::time_point expires; };
  std::chrono::milliseconds ttl_;
  size_t max_entries_;
  mutable std::mutex mu_;
  mutable std::deque<Entry> entries_;
};

// Construct a payload only after checking the transport gate and explicit
// consent, then send it with bounded I/O.  No request is attempted for a
// disabled/consent-denied call.  `cancellation` may be null; when supplied it
// is polled while connecting, writing and reading.
CloudTransportResult send_cloud_request(const Event &,
                                        const CloudTransportOptions &, bool consent,
                                        CloudCancellation *cancellation = nullptr,
                                        uint64_t request_id = 0);

// Lower-level variant for callers that already performed make_cloud_payload.
// It still requires an enabled transport, and is useful for deterministic
// loopback tests or a future background worker.
CloudTransportResult send_cloud_payload(const CloudPayload &,
                                        const CloudTransportOptions &, bool consent,
                                        CloudCancellation *cancellation = nullptr);

enum class CloudWorkerState { Disabled, Idle, Sending, Backoff, Paused, Stopped };
struct CloudWorkerStatus {
  CloudWorkerState state{CloudWorkerState::Disabled};
  size_t queued{};
  uint64_t sent{}, failed{}, retries{};
  std::string detail;
};

// Optional background scheduler. It owns the network call and never runs it
// on the input thread. The queue is bounded, semantic events are deduplicated,
// and transient failures are retried with a capped exponential backoff.
class CloudWorker {
 public:
  CloudWorker(std::string status_path = {});
  ~CloudWorker();
  CloudWorker(const CloudWorker &) = delete;
  CloudWorker &operator=(const CloudWorker &) = delete;
  void configure(CloudTransportOptions options, bool consent);
  void enqueue(const Event &event);
  void pause(bool value);
  void cancel();
  CloudWorkerStatus status() const;
  bool wait_until_idle(std::chrono::milliseconds timeout) const;
  std::vector<CloudCandidate> candidates(std::string_view pinyin,
                                         std::string_view context_token) const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct PersonalEntry { std::string pinyin, text, context; uint64_t count{}; };
struct LearningModel { LocalLanguageModel language_model; uint64_t version{}; std::vector<PersonalEntry> entries; std::unordered_map<std::string, int> text_preferences; std::unordered_map<std::string, std::vector<size_t>> exact_index; std::unordered_map<std::string, correction::RuleStore> corrections; };
// Optional domain lexicon, separate from the learned character model.
struct LocalSmallCandidate { std::string pinyin, text, context; int score{}; };
// A coherent read of the durable state.current snapshot.  The control center
// uses this instead of sampling each counter through separate filesystem
// reads, which could otherwise mix generations during a publication.
struct LearningStatus {
  uint64_t ack{}, clear_ack{}, rollback_ack{}, disable_ack{}, version{};
  size_t observed{}, learned{};
  bool paused{}, enabled{true}, rollback_ok{}, valid{};
  std::string status;
};
enum class ModelBackend { TiP, LocalSmall, Cloud };
struct ModelBackendInfo { ModelBackend backend{}; std::string name, description, run_when, privacy, status; bool available{}, selected{}; };
class ModelSettings {
 public:
  explicit ModelSettings(std::string dir = {});
  bool refresh(); ModelBackend selected() const; bool select(ModelBackend);
  std::vector<ModelBackendInfo> backends() const; bool cloud_enabled() const;
  bool set_cloud_api_key_path(std::string); std::string cloud_api_key_path() const;
  std::string path() const; std::string local_small_path() const;
  std::vector<LocalSmallCandidate> local_small_candidates(const std::string &raw, const std::string &context) const;
 private:
  // A staging instance performs refresh I/O without holding the mutex used
  // by the input thread. Only the final in-memory swap takes that mutex.
  struct Staging {};
  ModelSettings(std::string dir, Staging) : dir_(std::move(dir)) {}
  bool available(ModelBackend) const; static const char* id(ModelBackend);
  static std::optional<ModelBackend> parse_id(const std::string&);
  bool load_locked(); bool load_local_small_locked(); bool write_locked() const;
  std::string dir_; mutable std::mutex mu_; uint64_t revision_{};
  ModelBackend selected_{ModelBackend::TiP}; bool cloud_enabled_{};
  std::string cloud_api_key_path_; std::vector<LocalSmallCandidate> local_small_;
};
enum class LearnerMode { Automatic, Control };
class Learner {
public:
  explicit Learner(std::string dir = {}, LearnerMode mode = LearnerMode::Automatic);
  ~Learner();
  void push(Event);
  // Control operations return whether the request was durably acknowledged by
  // the writer.  Callers that do not need feedback may continue to ignore the
  // return value (the API remains source-compatible with the old void use).
  bool pause(bool);
  bool paused() const;
  bool set_enabled(bool);
  bool enabled() const;
  size_t observed() const;
  size_t learned() const;
  size_t queue_depth() const;
  size_t dropped_events() const;
  size_t publish_count() const;
  // CPU time spent in the writer and time deliberately yielded by its
  // duty-cycle budget. These counters are diagnostic only and never touch
  // the input hot path.
  uint64_t worker_cpu_us() const;
  uint64_t worker_throttled_us() const;
  uint64_t disk_bytes() const;
  bool export_data(const std::string&, std::stop_token cancellation = {}) const;
  bool clear();
  bool rollback();
  std::string status() const;
  LearningStatus status_snapshot() const;
  std::shared_ptr<const LearningModel> model() const;
  int preference(const std::string&) const;
  // Immutable, context-scoped lookup: at most 16 results, with at least three
  // observations of the canonical pinyin or the actual typed abbreviation.
  std::vector<PersonalEntry> exact_entries(const std::string& raw, const std::string& context) const;
  bool literal_preferred(const std::string& raw, const std::string& context) const;
  std::vector<std::string> correction_candidates(const std::string& raw, const std::string& context) const;
  bool wait_until_idle(std::chrono::milliseconds) const;
  CloudWorkerStatus cloud_status() const;
  // Returns only validated, short-lived candidates from the opt-in cloud
  // cache. The result is never persisted or merged into the TiP model.
  std::vector<CloudCandidate> cloud_candidates(std::string_view pinyin,
                                                std::string_view context) const;
  ModelSettings& models() { return *model_settings_; }
  const ModelSettings& models() const { return *model_settings_; }
private:
  struct Control {
    uint64_t revision{}, clear_revision{}, rollback_revision{}, disable_revision{};
    bool paused{}, enabled{true};
    bool valid{};
  };
  struct SharedState {
    uint64_t ack{}, clear_ack{}, rollback_ack{}, disable_ack{}, version{};
    size_t observed{}, learned{};
    bool paused{}, enabled{true}, rollback_ok{};
    std::string status;
  };
  struct Pending { PersonalEntry entry; uint64_t touched{}; std::string raw, remaining, corrected_from; bool inferred{}; };
  enum class Command { Pause, Enable, Clear, Rollback };
  void run();
  void throttle_worker(uint64_t batch_cpu_us);
  void process(const Event&);
  void publish(bool force = false, bool rotate = true);
  void load();
  bool load_file(const std::string&, bool install = true);
  void write_state();
  void apply_control(const Control&);
  bool command(Command, bool value = false);
  bool try_temporary_writer();
  void refresh_observer();
  void refresh_disk_bytes() const;
  Control read_control() const;
  std::optional<SharedState> read_state() const;
  std::string dir_;
  mutable std::mutex mu_, request_mu_;
  std::unique_ptr<ModelSettings> model_settings_;
  mutable std::condition_variable cv_, idle_cv_;
  std::deque<Event> q_;
  // Only the holder of writer.lock accesses mutable training state. Disk I/O
  // never holds mu_; input events only touch mu_ and immutable snapshots.
  std::unordered_map<std::string, PersonalEntry> counts_;
  std::unordered_map<uint64_t, Pending> pending_;
  std::unordered_map<std::string, correction::RuleStore> corrections_;
  std::atomic<bool> stop_{false}, paused_{false}, enabled_{true};
  // Set by a pause/disable request before taking request_mu_.  The worker
  // checks it between events and rolls the current batch back atomically.
  std::atomic<bool> cancel_batch_{false};
  std::thread worker_;
  int writer_fd_{-1};
  bool writer_{};
  mutable size_t observed_{};
  bool busy_{}, resetting_{};
  uint64_t gates_revision_{}, requested_revision_{};
  std::atomic<uint64_t> ack_{0};
  // Revision of the most recent clear requested by this instance.  It lets
  // observers ignore a still older state.current while the writer publishes
  // the acknowledged clear operation.
  mutable std::atomic<uint64_t> clear_floor_{0};
  // Last clear generation incorporated into observed_.  This lets a
  // long-lived read-only observer reset its local counter when another
  // control instance clears the shared model, while still accepting later
  // observations from the writer.
  mutable uint64_t clear_seen_{};
  mutable uint64_t clear_ack_{};
  uint64_t rollback_ack_{}, disable_ack_{}, generation_{};
  bool rollback_ok_{}, current_valid_{}, state_dirty_{};
  std::chrono::steady_clock::time_point retry_at_{};
  std::string status_{"等待输入"}, persisted_body_;
  std::atomic<size_t> dropped_{0}, publishes_{0};
  std::atomic<uint64_t> worker_cpu_us_{0}, worker_throttled_us_{0};
  mutable std::atomic<uint64_t> disk_bytes_{0};
  std::atomic<std::shared_ptr<const LearningModel>> snapshot_;
  std::unique_ptr<CloudWorker> cloud_worker_;
  CloudTransportOptions cloud_options_;
};
}
