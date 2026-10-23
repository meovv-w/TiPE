#include "tipe/learning.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tipe {
namespace {
const char *state_name(CloudWorkerState state) {
  switch (state) {
    case CloudWorkerState::Disabled: return "disabled";
    case CloudWorkerState::Idle: return "idle";
    case CloudWorkerState::Sending: return "sending";
    case CloudWorkerState::Backoff: return "backoff";
    case CloudWorkerState::Paused: return "paused";
    case CloudWorkerState::Stopped: return "stopped";
  }
  return "disabled";
}
bool retryable(CloudTransportStatus status) {
  return status == CloudTransportStatus::TimedOut ||
         status == CloudTransportStatus::TransportError ||
         status == CloudTransportStatus::ProtocolError;
}
}

struct CloudWorker::Impl {
  struct Item { CloudPayload payload; unsigned attempts{}; };
  mutable std::mutex mu;
  std::condition_variable cv, idle_cv;
  std::deque<Item> queue;
  CloudTransportOptions options;
  std::unique_ptr<CloudCancellation> active;
  std::string status_path;
  std::string detail;
  CloudWorkerState state{CloudWorkerState::Disabled};
  uint64_t sent{}, failed{}, retries{}, next_request_id{1};
  bool consent{}, paused{}, stop{}, dirty{true};
  uint64_t generation{};
  std::chrono::steady_clock::time_point retry_at{};
  CloudCandidateCache cache;
  std::thread worker;

  explicit Impl(std::string path) : status_path(std::move(path)) {
    worker = std::thread([this] { run(); });
  }
  ~Impl() {
    {
      std::lock_guard lock(mu);
      stop = true;
      if (active) active->cancel();
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
  }
  void persist_pending() {
    CloudWorkerStatus snapshot;
    {
      std::lock_guard lock(mu);
      if (!dirty) return;
      dirty = false;
      snapshot = {state, queue.size(), sent, failed, retries, detail};
    }
    if (status_path.empty()) return;
    std::error_code ec;
    const auto parent = std::filesystem::path(status_path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    const auto tmp = status_path + ".tmp";
    std::ofstream file(tmp, std::ios::trunc);
    if (!file) return;
    std::string safe_detail = snapshot.detail.substr(0, 256);
    for (char &ch : safe_detail) if (ch == '\t' || ch == '\r' || ch == '\n') ch = ' ';
    file << "#tipe-cloud-v1\t" << state_name(snapshot.state) << '\t' << snapshot.queued
         << '\t' << snapshot.sent << '\t' << snapshot.failed << '\t' << snapshot.retries << '\t'
         << safe_detail << '\n';
    file.close();
    if (!file) return;
    ::chmod(tmp.c_str(), 0600);
    std::filesystem::rename(tmp, status_path, ec);
  }
  void set_state_locked(CloudWorkerState value, std::string message = {}) {
    state = value;
    if (!message.empty()) detail = std::move(message);
    dirty = true;
  }
  void run() {
    (void)::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
    for (;;) {
      persist_pending();
      Item item;
      CloudTransportOptions request_options;
      bool request_consent = false;
      uint64_t request_generation = 0;
      {
        std::unique_lock lock(mu);
        if (queue.empty() || paused_unsafe()) {
          cv.wait(lock, [&] { return stop || dirty || (!queue.empty() && !paused_unsafe()); });
        } else {
          cv.wait_until(lock, retry_at, [&] { return stop || dirty || paused_unsafe(); });
        }
        if (stop) break;
        if (queue.empty() || paused_unsafe() || std::chrono::steady_clock::now() < retry_at) continue;
        item = std::move(queue.front()); queue.pop_front();
        active = std::make_unique<CloudCancellation>();
        request_options = options;
        request_consent = consent;
        request_generation = generation;
        set_state_locked(CloudWorkerState::Sending);
      }
      auto result = send_cloud_payload(item.payload, request_options, request_consent, active.get());
      {
        std::lock_guard lock(mu);
        active.reset();
        if (stop) break;
        if (!result.ok()) cache.clear();
        if (request_generation != generation) {
          idle_cv.notify_all();
          continue;
        }
        if (!options.enabled || !consent) {
          set_state_locked(CloudWorkerState::Disabled, "云端增强未启用");
        } else if (result.ok()) {
          std::vector<CloudCandidate> candidates;
          std::string parse_detail;
          // A transport-level 2xx is not enough to publish remote data. Only
          // the bounded schema parser can place candidates in the ephemeral
          // cache; malformed responses are treated as a failed request.
          if (!parse_cloud_candidates(result.response, item.payload.request_id,
                                      candidates, &parse_detail)) {
            cache.clear();
            ++failed;
            detail = parse_detail.empty() ? "云端响应格式无效" : parse_detail;
            retry_at = {};
            set_state_locked(CloudWorkerState::Idle);
            idle_cv.notify_all();
            continue;
          }
          if (!candidates.empty()) cache.put(item.payload.request_id, item.payload.context_token,
                                              std::move(candidates));
          ++sent;
          // At most one successful request every five seconds, regardless
          // of typing speed. Failures use their separate bounded retry path.
          retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
          set_state_locked(CloudWorkerState::Idle, "已发送");
        } else if (result.status == CloudTransportStatus::Cancelled && paused) {
          if (queue.size() >= 64) queue.pop_back();
          queue.push_front(std::move(item));
          set_state_locked(CloudWorkerState::Paused, "已暂停");
        } else if (retryable(result.status) && item.attempts < 3 && !paused) {
          ++item.attempts; ++retries;
          queue.push_front(std::move(item));
          const auto delay = std::chrono::milliseconds(200 * (1u << (item.attempts - 1)));
          retry_at = std::chrono::steady_clock::now() + delay;
          set_state_locked(CloudWorkerState::Backoff, "下次自动重试");
        } else {
          ++failed;
          detail = result.detail.empty() ? "云端请求失败" : result.detail;
          retry_at = {};
          set_state_locked(paused ? CloudWorkerState::Paused : CloudWorkerState::Idle);
        }
        idle_cv.notify_all();
      }
    }
    {
      std::lock_guard lock(mu);
      state = CloudWorkerState::Stopped;
      dirty = true;
      idle_cv.notify_all();
    }
    persist_pending();
  }
  bool paused_unsafe() const { return paused || !options.enabled || !consent; }
};

CloudWorker::CloudWorker(std::string status_path)
    : impl_(std::make_unique<Impl>(std::move(status_path))) {}
CloudWorker::~CloudWorker() = default;

void CloudWorker::configure(CloudTransportOptions options, bool consent) {
  std::lock_guard lock(impl_->mu);
  impl_->options = std::move(options);
  impl_->consent = consent;
  ++impl_->generation;
  if (impl_->active) impl_->active->cancel();
  impl_->queue.clear();
  impl_->cache.clear();
  impl_->retry_at = {};
  if (!impl_->options.enabled || impl_->options.endpoint.empty() || !consent) {
    impl_->options.enabled = false;
    impl_->set_state_locked(CloudWorkerState::Disabled, "云端增强未启用");
  } else {
    impl_->set_state_locked(impl_->paused ? CloudWorkerState::Paused : CloudWorkerState::Idle,
                             "等待后台处理");
  }
  impl_->cv.notify_all();
}

void CloudWorker::enqueue(const Event &event) {
  if (event.sensitive || (event.type != EventType::CandidateSelect && event.type != EventType::Commit)) return;
  if (event.pinyin.empty() || event.committed.empty()) return;
  std::lock_guard lock(impl_->mu);
  if (!impl_->options.enabled || !impl_->consent || impl_->paused || impl_->stop) return;
  auto payload = make_cloud_payload(event, true, impl_->next_request_id++);
  if (!payload) return;
  // Keep only one pending copy of an identical semantic event. This avoids
  // turning repeated UI updates into a network stream.
  const auto same = [&](const Impl::Item &item) {
    return item.payload.pinyin == payload->pinyin &&
           item.payload.committed == payload->committed && item.payload.context_token == payload->context_token;
  };
  if (std::any_of(impl_->queue.begin(), impl_->queue.end(), same)) return;
  if (impl_->queue.size() >= 64) impl_->queue.pop_front();
  impl_->queue.push_back({std::move(*payload), 0});
  impl_->set_state_locked(CloudWorkerState::Idle, "等待后台处理");
  impl_->cv.notify_one();
}

void CloudWorker::pause(bool value) {
  std::lock_guard lock(impl_->mu);
  if (impl_->paused == value) return;
  impl_->paused = value;
  if (value) {
    if (impl_->active) impl_->active->cancel();
    impl_->set_state_locked(CloudWorkerState::Paused, "已暂停");
  } else {
    impl_->set_state_locked(impl_->options.enabled && impl_->consent
                                ? CloudWorkerState::Idle : CloudWorkerState::Disabled,
                            "等待后台处理");
  }
  impl_->cv.notify_all();
}

void CloudWorker::cancel() {
  std::lock_guard lock(impl_->mu);
  ++impl_->generation;
  impl_->queue.clear();
  impl_->cache.clear();
  if (impl_->active) impl_->active->cancel();
  impl_->retry_at = {};
  if (impl_->options.enabled && impl_->consent && !impl_->paused)
    impl_->set_state_locked(CloudWorkerState::Idle, "已取消待发送任务");
  impl_->idle_cv.notify_all();
  impl_->cv.notify_all();
}

CloudWorkerStatus CloudWorker::status() const {
  std::lock_guard lock(impl_->mu);
  return {impl_->state, impl_->queue.size(), impl_->sent, impl_->failed,
          impl_->retries, impl_->detail};
}

bool CloudWorker::wait_until_idle(std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->mu);
  return impl_->idle_cv.wait_for(lock, timeout, [&] {
    return impl_->queue.empty() && !impl_->active;
  });
}
std::vector<CloudCandidate> CloudWorker::candidates(std::string_view pinyin,
                                                    std::string_view context_token) const {
  return impl_->cache.lookup(pinyin, context_token);
}
} // namespace tipe
