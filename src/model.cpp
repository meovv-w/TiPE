#include "tipe/learning.hpp"
#include "cloud_validation.hpp"

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string_view>

namespace fs = std::filesystem;

namespace tipe {
namespace {
uint64_t fnv64(std::string_view value) {
  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string default_state_dir() {
  if (const char *state = std::getenv("XDG_STATE_HOME"); state && *state)
    return std::string(state) + "/tipe";
  if (const char *home = std::getenv("HOME"); home && *home)
    return std::string(home) + "/.local/state/tipe";
  return ".tipe";
}

std::string clean_field(std::string value) {
  for (char &c : value)
    if (c == '\t' || c == '\r' || c == '\n') c = ' ';
  return value;
}
} // namespace

std::optional<CloudPayload> make_cloud_payload(const Event &event,
                                                bool consent,
                                                uint64_t request_id) {
  // Cloud enhancement is an explicit, semantic-event-only opt-in.  In
  // particular, never turn a key-down event or a sensitive-field event into
  // a request by accident.
  if (!consent || event.sensitive ||
      (event.type != EventType::CandidateSelect &&
       event.type != EventType::Commit) ||
      event.pinyin.empty() || event.committed.empty() ||
      !cloud_detail::valid_pinyin(event.pinyin) ||
      !cloud_detail::valid_text(event.committed, 512) ||
      event.pinyin.find_first_of("\t\r\n") != std::string::npos ||
      event.committed.find_first_of("\t\r\n") != std::string::npos ||
      event.context.size() > 256 || !cloud_detail::valid_text(event.context, 256, true) ||
      event.context.find_first_of("\t\r\n") != std::string::npos)
    return std::nullopt;
  CloudPayload payload;
  payload.pinyin = event.pinyin;
  payload.committed = event.committed;
  // Send a one-way context token rather than an application name or path.
  payload.context_token = std::to_string(fnv64(event.context));
  payload.interval_ms = event.interval_ms;
  payload.delete_interval_ms = event.delete_interval_ms;
  payload.request_id = request_id;
  return payload;
}

ModelSettings::ModelSettings(std::string dir) : dir_(std::move(dir)) {
  if (dir_.empty()) dir_ = default_state_dir();
  std::error_code ec;
  fs::create_directories(dir_, ec);
  ::chmod(dir_.c_str(), 0700);
  std::lock_guard lock(mu_);
  load_local_small_locked();
  if (!load_locked()) write_locked();
}

const char *ModelSettings::id(ModelBackend backend) {
  switch (backend) {
    case ModelBackend::TiP: return "tip";
    case ModelBackend::LocalSmall: return "local-small";
    case ModelBackend::Cloud: return "cloud";
  }
  return "tip";
}

std::optional<ModelBackend> ModelSettings::parse_id(const std::string &value) {
  if (value == "tip") return ModelBackend::TiP;
  if (value == "local-small") return ModelBackend::LocalSmall;
  if (value == "cloud") return ModelBackend::Cloud;
  return std::nullopt;
}

bool ModelSettings::available(ModelBackend backend) const {
  // TiP is the local incremental preference layer implemented by Learner.
  // LocalSmall is compiled in and starts with an empty personal character
  // model. Cloud is available only when the user configured the complete
  // opt-in gate. Without all three values the UI remains visibly disabled and
  // a persisted hand-edited model selection cannot activate networking.
  if (backend == ModelBackend::TiP) return true;
  if (backend == ModelBackend::LocalSmall) return true;
  const char *enabled = std::getenv("TIPE_CLOUD_ENABLED");
  const char *consent = std::getenv("TIPE_CLOUD_CONSENT");
  const char *endpoint = std::getenv("TIPE_CLOUD_ENDPOINT");
  return enabled && std::string_view(enabled) == "1" && consent &&
         std::string_view(consent) == "1" && endpoint && *endpoint;
}

bool ModelSettings::load_local_small_locked() {
  local_small_.clear();
  // The optional file is an explicit domain lexicon, separate from the
  // automatically trained statistical model. No demonstration word table.
  std::ifstream file(dir_ + "/local-small.tsv");
  if (!file) return true;
  std::error_code ec;
  if(fs::file_size(dir_+"/local-small.tsv",ec)>1024*1024 || ec) return true;
  std::string line;
  size_t rows = 0;
  while (std::getline(file, line) && rows < 4096) {
    if(line.size()>2048) continue;
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> fields;
    size_t begin = 0;
    while (true) {
      const size_t tab = line.find('\t', begin);
      if (tab == std::string::npos) { fields.push_back(line.substr(begin)); break; }
      fields.push_back(line.substr(begin, tab - begin)); begin = tab + 1;
    }
    if (fields.size() < 2 || fields.size() > 4 || fields[0].empty() ||
        fields[1].empty() || fields[0].size() > 128 || fields[1].size() > 512)
      continue;
    bool pinyin_ok = true;
    for (unsigned char c : fields[0])
      if (!((c >= 'a' && c <= 'z') || c == '\'')) { pinyin_ok = false; break; }
    if (!pinyin_ok) continue;
    int score = 0;
    try { score = fields.size() >= 3 && !fields[2].empty() ? std::stoi(fields[2]) : 0; }
    catch (...) { continue; }
    score = std::clamp(score, -100000, 100000);
    std::string context = fields.size() == 4 ? clean_field(fields[3]) : std::string{};
    if (context.size() > 512) continue;
    local_small_.push_back({fields[0], fields[1], std::move(context), score});
    ++rows;
  }
  std::stable_sort(local_small_.begin(), local_small_.end(),
                   [](const auto &a, const auto &b) { return a.score > b.score; });
  return true;
}

bool ModelSettings::load_locked() {
  selected_ = ModelBackend::TiP;
  cloud_enabled_ = false;
  cloud_api_key_path_.clear();
  std::ifstream file(dir_ + "/models.current");
  if (!file) return false;
  std::string line;
  if (!std::getline(file, line)) return false;
  std::vector<std::string> fields;
  size_t begin = 0;
  while (true) {
    const size_t tab = line.find('\t', begin);
    if (tab == std::string::npos) {
      fields.push_back(line.substr(begin));
      break;
    }
    fields.push_back(line.substr(begin, tab - begin));
    begin = tab + 1;
  }
  // Format: #tipe-model-v1, selected backend id, cloud enabled, API key path.
  // Malformed or unavailable values safely fall back to TiP and disabled
  // cloud requests.
  if (fields.size() < 4 || fields[0] != "#tipe-model-v1") return false;
  // Preserve the harmless API-key path while rejecting an unavailable or
  // unknown selected backend.  The caller then rewrites a canonical TiP
  // configuration atomically, so a stale hand-edited file cannot persist.
  cloud_api_key_path_ = fields[3];
  auto parsed = parse_id(fields[1]);
  if (!parsed || !available(*parsed)) return false;
  selected_ = *parsed;
  cloud_enabled_ = fields[2] == "1" && available(ModelBackend::Cloud);
  return true;
}

bool ModelSettings::write_locked() const {
  const std::string path = dir_ + "/models.current";
  const std::string tmp = path + ".tmp";
  std::ofstream file(tmp, std::ios::trunc);
  if (!file) return false;
  // Keep the persisted cloud bit gated by the current explicit environment
  // configuration; a stale file must never activate networking by itself.
  const bool cloud = cloud_enabled_ && available(ModelBackend::Cloud);
  file << "#tipe-model-v1\t" << id(selected_) << '\t' << (cloud ? 1 : 0)
       << '\t' << clean_field(cloud_api_key_path_) << '\n';
  file.close();
  if (!file) { std::error_code ignored; fs::remove(tmp, ignored); return false; }
  ::chmod(tmp.c_str(), 0600);
  const int fd = ::open(tmp.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0 || ::fsync(fd) != 0) {
    if (fd >= 0) ::close(fd);
    std::error_code ignored; fs::remove(tmp, ignored); return false;
  }
  ::close(fd);
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) { fs::remove(tmp, ec); return false; }
  const int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
  return !ec;
}

bool ModelSettings::refresh() {
  uint64_t revision;
  { std::lock_guard lock(mu_); revision = revision_; }
  ModelSettings staged(dir_, Staging{});
  staged.load_local_small_locked();
  const bool loaded = staged.load_locked();
  if (!loaded) staged.write_locked();
  std::lock_guard lock(mu_);
  if (revision != revision_) return loaded;
  selected_ = staged.selected_; cloud_enabled_ = staged.cloud_enabled_;
  cloud_api_key_path_ = std::move(staged.cloud_api_key_path_);
  local_small_ = std::move(staged.local_small_);
  return loaded;
}

ModelBackend ModelSettings::selected() const {
  std::lock_guard lock(mu_);
  return selected_;
}

bool ModelSettings::select(ModelBackend backend) {
  std::lock_guard lock(mu_);
  if (!available(backend)) return false;
  ++revision_;
  if (selected_ == backend) {
    cloud_enabled_ = backend == ModelBackend::Cloud;
    write_locked();
    return true;
  }
  selected_ = backend;
  cloud_enabled_ = backend == ModelBackend::Cloud;
  return write_locked();
}

std::vector<ModelBackendInfo> ModelSettings::backends() const {
  std::lock_guard lock(mu_);
  std::vector<ModelBackendInfo> result;
  result.push_back({ModelBackend::TiP, "TiP（个人学习）",
                    "根据你的选词和提交结果，安静地整理个人偏好。",
                    "输入后的后台低优先级整理；不需要手动训练。",
                    "只保存在本机，不发送键盘数据。", "可用", true,
                    selected_ == ModelBackend::TiP});
  result.push_back({ModelBackend::LocalSmall, "本地小模型",
                    "从反复选择的词句中学习字符搭配，调整后续候选排序；可另加领域词表。",
                    "后台自动学习；输入时在内存中评分，保留基础词库首选。",
                    "数据和模型文件均留在本机。", available(ModelBackend::LocalSmall)
                      ? "可用（本机自动学习）" : "未安装本地模型",
                    available(ModelBackend::LocalSmall), selected_ == ModelBackend::LocalSmall});
  result.push_back({ModelBackend::Cloud, "云端大模型",
                    "可选的云端候选增强，只使用已验证的短期结果。",
                    "后台发送选词/提交语义事件；候选缓存有时间和容量上限。",
                    "只发送提交/选词语义事件，不发送原始键盘流。",
                    available(ModelBackend::Cloud) ? "已配置，可用" : "未配置或未同意", available(ModelBackend::Cloud),
                    selected_ == ModelBackend::Cloud});
  return result;
}

bool ModelSettings::cloud_enabled() const {
  std::lock_guard lock(mu_);
  return cloud_enabled_ && available(ModelBackend::Cloud);
}

bool ModelSettings::set_cloud_api_key_path(std::string path) {
  // A path is harmless metadata and can be configured ahead of a future
  // backend.  It never enables cloud requests or reads the secret.
  if (path.size() > 4096) return false;
  std::lock_guard lock(mu_);
  ++revision_;
  cloud_api_key_path_ = clean_field(std::move(path));
  return write_locked();
}

std::string ModelSettings::cloud_api_key_path() const {
  std::lock_guard lock(mu_);
  return cloud_api_key_path_;
}

std::string ModelSettings::path() const {
  std::lock_guard lock(mu_);
  return dir_ + "/models.current";
}

std::string ModelSettings::local_small_path() const {
  std::lock_guard lock(mu_);
  return dir_ + "/local-small.tsv";
}

std::vector<LocalSmallCandidate> ModelSettings::local_small_candidates(
    const std::string &raw, const std::string &context) const {
  std::lock_guard lock(mu_);
  std::vector<LocalSmallCandidate> out;
  if (!available(ModelBackend::LocalSmall)) return out;
  for (const auto &candidate : local_small_) {
    if (candidate.pinyin != raw) continue;
    if (!candidate.context.empty() && candidate.context != context) continue;
    out.push_back(candidate);
    if (out.size() >= 32) break;
  }
  return out;
}
} // namespace tipe
