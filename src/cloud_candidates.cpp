#include "tipe/learning.hpp"
#include "cloud_validation.hpp"

#include <algorithm>
#include <charconv>
#include <unordered_set>

namespace tipe {
namespace {
constexpr size_t max_response = 64 * 1024;
constexpr size_t max_candidates = 32;
constexpr size_t max_text = 256;
std::string cache_key(std::string_view pinyin) {
  std::string key;
  key.reserve(pinyin.size());
  for (char ch : pinyin) if (ch != '\'') key.push_back(ch);
  return key;
}

bool valid_candidate(const CloudCandidate &candidate) {
  return cloud_detail::valid_pinyin(candidate.pinyin) &&
         cloud_detail::valid_text(candidate.text, max_text) &&
         candidate.score >= 0 && candidate.score <= 1000;
}

// A non-recursive parser for this one schema. No arbitrary object tree or
// unbounded allocation is built from remote data.
class Parser {
 public:
  explicit Parser(std::string_view source) : source_(source) {}
  bool response(uint64_t expected, std::vector<CloudCandidate> &out) {
    if (!consume('{')) return false;
    bool have_id = false, have_candidates = false;
    if (consume('}')) return end(); // Transport-only acknowledgement.
    do {
      std::string key;
      if (!string(key, 16) || !consume(':')) return false;
      if (key == "request_id") {
        if (have_id) return false;
        have_id = true;
        uint64_t id{};
        if (!integer(id) || id == 0 || id != expected) return false;
      } else if (key == "candidates") {
        if (have_candidates) return false;
        have_candidates = true;
        if (!array(out)) return false;
      } else return false;
      if (consume('}')) return end();
    } while (consume(','));
    return false;
  }
 private:
  void space() {
    while (cursor_ < source_.size() && (source_[cursor_] == ' ' || source_[cursor_] == '\t' ||
           source_[cursor_] == '\r' || source_[cursor_] == '\n')) ++cursor_;
  }
  bool end() { space(); return cursor_ == source_.size(); }
  bool consume(char c) {
    space();
    if (cursor_ == source_.size() || source_[cursor_] != c) return false;
    ++cursor_; return true;
  }
  template<class Number> bool integer(Number &out) {
    space();
    const size_t begin = cursor_;
    while (cursor_ < source_.size() && source_[cursor_] >= '0' && source_[cursor_] <= '9') ++cursor_;
    if (cursor_ == begin || (cursor_ - begin > 1 && source_[begin] == '0')) return false;
    const auto result = std::from_chars(source_.data() + begin, source_.data() + cursor_, out);
    return result.ec == std::errc{} && result.ptr == source_.data() + cursor_;
  }
  bool hex(uint32_t &cp) {
    if (cursor_ + 4 > source_.size()) return false;
    cp = 0;
    for (unsigned i = 0; i < 4; ++i) {
      const unsigned char ch = source_[cursor_++];
      unsigned digit;
      if (ch >= '0' && ch <= '9') digit = ch - '0';
      else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
      else return false;
      cp = (cp << 4) | digit;
    }
    return true;
  }
  void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xc0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
    else if (cp < 0x10000) { out.push_back(static_cast<char>(0xe0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
    else { out.push_back(static_cast<char>(0xf0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
  }
  bool string(std::string &out, size_t limit) {
    if (!consume('"')) return false;
    out.clear();
    while (cursor_ < source_.size()) {
      const unsigned char ch = source_[cursor_++];
      if (ch == '"') return cloud_detail::valid_text(out, limit, true);
      if (ch < 0x20) return false;
      if (ch != '\\') out.push_back(static_cast<char>(ch));
      else {
        if (cursor_ == source_.size()) return false;
        const char escape = source_[cursor_++];
        if (escape == '"' || escape == '\\' || escape == '/') out.push_back(escape);
        else if (escape == 'u') {
          uint32_t cp{};
          if (!hex(cp)) return false;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (cursor_ + 2 > source_.size() || source_[cursor_] != '\\' || source_[cursor_ + 1] != 'u') return false;
            cursor_ += 2; uint32_t low{};
            if (!hex(low) || low < 0xdc00 || low > 0xdfff) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
          } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
          append_utf8(out, cp);
        } else return false; // Escaped controls are not permitted in fields.
      }
      if (out.size() > limit) return false;
    }
    return false;
  }
  bool candidate(CloudCandidate &out) {
    if (!consume('{')) return false;
    bool pinyin = false, text = false, score = false;
    do {
      std::string key;
      if (!string(key, 8) || !consume(':')) return false;
      if (key == "pinyin") {
        if (pinyin || !string(out.pinyin, 128)) return false;
        pinyin = true;
      } else if (key == "text") {
        if (text || !string(out.text, max_text)) return false;
        text = true;
      } else if (key == "score") {
        if (score || !integer(out.score)) return false;
        score = true;
      } else return false;
      if (consume('}')) return pinyin && text && score && valid_candidate(out);
    } while (consume(','));
    return false;
  }
  bool array(std::vector<CloudCandidate> &out) {
    if (!consume('[')) return false;
    if (consume(']')) return true;
    do {
      if (out.size() >= max_candidates) return false;
      CloudCandidate item;
      if (!candidate(item)) return false;
      if (std::any_of(out.begin(), out.end(), [&](const auto &old) { return old.pinyin == item.pinyin && old.text == item.text; })) return false;
      out.push_back(std::move(item));
      if (consume(']')) return true;
    } while (consume(','));
    return false;
  }
  std::string_view source_;
  size_t cursor_{};
};
} // namespace

bool parse_cloud_candidates(std::string_view response, uint64_t expected_request_id,
                            std::vector<CloudCandidate> &out, std::string *detail) {
  out.clear();
  std::vector<CloudCandidate> parsed;
  if (response.empty() || response.size() > max_response || !Parser(response).response(expected_request_id, parsed)) {
    if (detail) *detail = "invalid bounded cloud candidate response";
    return false;
  }
  out = std::move(parsed);
  if (detail) detail->clear();
  return true;
}

CloudCandidateCache::CloudCandidateCache(std::chrono::milliseconds ttl, size_t max_entries)
    : ttl_(std::clamp(ttl, std::chrono::milliseconds(1), std::chrono::milliseconds(300000))),
      max_entries_(std::clamp<size_t>(max_entries, 1, 128)) {}

bool CloudCandidateCache::put(uint64_t request_id, std::string context_token,
                              std::vector<CloudCandidate> candidates) {
  if (!cloud_detail::valid_context_token(context_token) || candidates.size() > max_candidates ||
      !std::all_of(candidates.begin(), candidates.end(), valid_candidate)) return false;
  std::unordered_set<std::string> unique;
  for (const auto &candidate : candidates)
    if (!unique.insert(candidate.pinyin + '\0' + candidate.text).second) return false;
  // Validate the whole batch before changing any cached entry.
  std::lock_guard lock(mu_);
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(entries_, [&](const Entry &entry) { return entry.expires <= now; });
  std::vector<std::string> pinyins;
  for (const auto &candidate : candidates)
    if (std::find(pinyins.begin(), pinyins.end(), candidate.pinyin) == pinyins.end()) pinyins.push_back(candidate.pinyin);
  for (const auto &pinyin : pinyins) {
    const auto key = cache_key(pinyin);
    const auto old = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry) { return entry.pinyin == key && entry.context == context_token; });
    // Late responses may not replace a more recent request for this key.
    if (old != entries_.end() && old->request_id > request_id) continue;
    if (old != entries_.end()) entries_.erase(old);
    Entry entry{request_id, key, context_token, {}, now + ttl_};
    for (const auto &candidate : candidates) if (candidate.pinyin == pinyin) entry.candidates.push_back(candidate);
    std::stable_sort(entry.candidates.begin(), entry.candidates.end(), [](const auto &a, const auto &b) { return a.score > b.score; });
    while (entries_.size() >= max_entries_) entries_.pop_front();
    entries_.push_back(std::move(entry));
  }
  return true;
}

std::vector<CloudCandidate> CloudCandidateCache::lookup(std::string_view pinyin, std::string_view context_token) const {
  if (!cloud_detail::valid_pinyin(pinyin) || !cloud_detail::valid_context_token(context_token)) return {};
  std::lock_guard lock(mu_);
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(entries_, [&](const Entry &entry) { return entry.expires <= now; });
  const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry) { return entry.pinyin == cache_key(pinyin) && entry.context == context_token; });
  return found == entries_.end() ? std::vector<CloudCandidate>{} : found->candidates;
}

void CloudCandidateCache::clear() { std::lock_guard lock(mu_); entries_.clear(); }
size_t CloudCandidateCache::size() const {
  std::lock_guard lock(mu_);
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(entries_, [&](const Entry &entry) { return entry.expires <= now; });
  return entries_.size();
}
} // namespace tipe
