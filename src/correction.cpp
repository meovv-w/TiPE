#include "tipe/correction.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace tipe::correction {
namespace {

constexpr char kStart = '^';
constexpr char kEnd = '$';

bool ascii_letter(char c) noexcept { return c >= 'a' && c <= 'z'; }

bool valid_pinyin(std::string_view value, std::size_t max_length) noexcept {
  if (value.empty() || value.size() > max_length) return false;
  return std::all_of(value.begin(), value.end(), ascii_letter);
}

std::optional<Rule> identify(std::string_view raw, std::string_view corrected) {
  if (!valid_pinyin(raw, RuleStore::kMaxRawLength) ||
      !valid_pinyin(corrected, RuleStore::kMaxRawLength + 1) || raw == corrected)
    return std::nullopt;
  std::optional<Rule> found;
  auto remember = [&](EditKind kind, std::size_t pos, std::size_t raw_length,
                      char replacement, char typed = 0) {
    if (found) return false;
    Rule r;
    r.kind = kind; r.missing = replacement; r.typed = typed;
    r.left = pos == 0 ? kStart : raw[pos - 1];
    r.right = pos + raw_length == raw.size() ? kEnd : raw[pos + raw_length];
    found = std::move(r);
    return true;
  };
  if (corrected.size() == raw.size() + 1) {
    for (std::size_t p = 0; p <= raw.size(); ++p) {
      if (corrected.substr(0,p) == raw.substr(0,p) && corrected.substr(p+1) == raw.substr(p))
        if (!remember(EditKind::Omission,p,0,corrected[p])) return std::nullopt;
    }
  } else if (raw.size() == corrected.size() + 1) {
    for (std::size_t p = 0; p < raw.size(); ++p) {
      if (raw.substr(0,p) == corrected.substr(0,p) && raw.substr(p+1) == corrected.substr(p))
        if (!remember(EditKind::Extra,p,1,raw[p])) return std::nullopt;
    }
  } else if (raw.size() == corrected.size()) {
    std::size_t p = 0;
    while (p < raw.size() && raw[p] == corrected[p]) ++p;
    if (p == raw.size()) return std::nullopt;
    if (raw.substr(p+1) == corrected.substr(p+1)) {
      remember(EditKind::Substitution,p,1,corrected[p],raw[p]);
    } else if (p+1 < raw.size() && raw[p] == corrected[p+1] && raw[p+1] == corrected[p] &&
               raw.substr(p+2) == corrected.substr(p+2)) {
      remember(EditKind::Transposition,p,2,raw[p],raw[p+1]);
    }
  }
  return found;
}

}  // namespace

bool Rule::active() const noexcept {
  return support >= RuleStore::kActivationSupport &&
         corrected_pinyin.size() >= RuleStore::kActivationVariants;
}

bool RuleStore::valid_raw(std::string_view value) noexcept {
  return valid_pinyin(value, kMaxRawLength);
}

bool RuleStore::valid_rule(const Rule& rule) noexcept {
  if (!rule.support || !ascii_letter(rule.missing)) return false;
  if (rule.kind != EditKind::Omission && rule.kind != EditKind::Extra &&
      rule.kind != EditKind::Substitution && rule.kind != EditKind::Transposition) return false;
  if ((rule.kind == EditKind::Substitution || rule.kind == EditKind::Transposition) &&
      (!ascii_letter(rule.typed) || rule.typed == rule.missing)) return false;
  if ((rule.kind == EditKind::Omission || rule.kind == EditKind::Extra) && rule.typed) return false;
  if ((rule.left != '^' && !ascii_letter(rule.left)) ||
      (rule.right != '$' && !ascii_letter(rule.right))) {
    return false;
  }
  // An external record may contain more entries; absorb() clips them.  The
  // key and support still have to be valid before merging it.
  return true;
}

bool RuleStore::same_key(const Rule& a, const Rule& b) noexcept {
  return a.kind == b.kind && a.typed == b.typed && a.missing == b.missing && a.left == b.left && a.right == b.right;
}

bool RuleStore::contains(const std::vector<std::string>& values,
                         std::string_view value) noexcept {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void RuleStore::add_bounded(std::vector<std::string>& values,
                            std::string_view value, std::size_t limit) {
  if (value.empty() || values.size() >= limit || contains(values, value)) return;
  values.emplace_back(value);
}

bool RuleStore::observe(std::string_view raw, std::string_view corrected) {
  const auto deletion = identify(raw, corrected);
  if (!deletion) return false;
  Rule key = *deletion;
  auto it = std::find_if(rules_.begin(), rules_.end(),
                         [&](const Rule& rule) { return same_key(rule, key); });
  if (it == rules_.end()) {
    if (rules_.size() >= kMaxRules) return false;
    rules_.push_back(std::move(key));
    it = std::prev(rules_.end());
  }
  if (it->support != std::numeric_limits<std::uint64_t>::max()) ++it->support;
  add_bounded(it->corrected_pinyin, corrected, kMaxVariantsPerRule);
  add_bounded(it->observed_raw, raw, kMaxVariantsPerRule);
  return true;
}

void RuleStore::absorb(const Rule& incoming) {
  if (!valid_rule(incoming)) return;
  auto it = std::find_if(rules_.begin(), rules_.end(),
                         [&](const Rule& rule) { return same_key(rule, incoming); });
  if (it == rules_.end()) {
    if (rules_.size() >= kMaxRules) return;
    Rule copy;
    copy.kind = incoming.kind;
    copy.typed = incoming.typed;
    copy.missing = incoming.missing;
    copy.left = incoming.left;
    copy.right = incoming.right;
    copy.support = incoming.support;
    for (const auto& value : incoming.corrected_pinyin) {
      if (valid_pinyin(value, kMaxRawLength + 1))
        add_bounded(copy.corrected_pinyin, value, kMaxVariantsPerRule);
    }
    for (const auto& value : incoming.observed_raw) {
      if (valid_raw(value)) add_bounded(copy.observed_raw, value, kMaxVariantsPerRule);
    }
    rules_.push_back(std::move(copy));
    return;
  }
  const auto max = std::numeric_limits<std::uint64_t>::max();
  if (max - it->support < incoming.support) it->support = max;
  else it->support += incoming.support;
  for (const auto& value : incoming.corrected_pinyin) {
    if (valid_pinyin(value, kMaxRawLength + 1))
      add_bounded(it->corrected_pinyin, value, kMaxVariantsPerRule);
  }
  for (const auto& value : incoming.observed_raw) {
    if (valid_raw(value)) add_bounded(it->observed_raw, value, kMaxVariantsPerRule);
  }
}

double RuleStore::confidence(const Rule &rule) const noexcept {
  long double competing = 0;
  for (const auto &other : rules_) {
    if (same_key(rule,other) || rule.kind != other.kind || rule.left != other.left || rule.right != other.right)
      continue;
    const bool same_input = rule.kind == EditKind::Omission ||
        (rule.kind == EditKind::Substitution && rule.typed == other.typed);
    if (same_input) competing += other.support;
  }
  return static_cast<double>((static_cast<long double>(rule.support) + 1) /
      (static_cast<long double>(rule.support) + competing + 2));
}

std::vector<std::string> RuleStore::generate(std::string_view raw,
                                             bool include_observed) const {
  std::vector<std::string> out;
  if (!valid_raw(raw)) return out;
  std::vector<const Rule *> active;
  for (const auto &rule : rules_)
    if (rule.active() && confidence(rule) >= 0.8 &&
        (include_observed || !contains(rule.observed_raw, raw))) active.push_back(&rule);
  std::stable_sort(active.begin(),active.end(),[&](const Rule *a,const Rule *b) {
    const auto ac=confidence(*a),bc=confidence(*b);
    return ac != bc ? ac > bc : a->support > b->support;
  });
  for (const auto *rp : active) {
    const auto &rule = *rp;
    const std::size_t length = rule.kind == EditKind::Omission ? 0 :
                               rule.kind == EditKind::Transposition ? 2 : 1;
    for (std::size_t p = 0; p + length <= raw.size() && out.size() < kMaxGenerated; ++p) {
      const char left = p == 0 ? kStart : raw[p - 1];
      const char right = p + length == raw.size() ? kEnd : raw[p + length];
      if (left != rule.left || right != rule.right) continue;
      if (rule.kind == EditKind::Extra && raw[p] != rule.missing) continue;
      if (rule.kind == EditKind::Substitution && raw[p] != rule.typed) continue;
      if (rule.kind == EditKind::Transposition &&
          (raw[p] != rule.missing || raw[p+1] != rule.typed)) continue;
      std::string candidate(raw.substr(0,p));
      if (rule.kind == EditKind::Omission || rule.kind == EditKind::Substitution)
        candidate.push_back(rule.missing);
      if (rule.kind == EditKind::Transposition) {
        candidate.push_back(rule.typed); candidate.push_back(rule.missing);
      }
      candidate.append(raw.substr(p+length));
      if (valid_pinyin(candidate,kMaxRawLength+1) && !contains(out,candidate))
        out.push_back(std::move(candidate));
    }
    if (out.size() >= kMaxGenerated) break;
  }
  return out;
}

}  // namespace tipe::correction
