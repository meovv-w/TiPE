#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tipe::correction {

enum class EditKind { Omission, Extra, Substitution, Transposition };

// A deletion rule says that one letter was omitted between left and right.
// '^' and '$' are boundary sentinels; they are not accepted as input letters.
// All fields are deliberately plain data so a learner can serialize them
// without depending on this implementation.
struct Rule {
  char missing{};
  char left{};
  char right{};
  std::uint64_t support{};
  std::vector<std::string> corrected_pinyin;
  // Raw pinyins used as evidence.  This bounded list lets generate() honour
  // its "unseen raw" contract while keeping a serialized model finite.
  std::vector<std::string> observed_raw;

  // Old snapshots contain omissions only. Additional fields describe a
  // single observed edit; boundary context excludes the edited characters.
  EditKind kind{EditKind::Omission};
  char typed{}; // Raw substituted letter, or second transposed letter.
  bool active() const noexcept;
};

class RuleStore {
 public:
  static constexpr std::size_t kMaxRules = 128;
  static constexpr std::size_t kMaxVariantsPerRule = 16;
  static constexpr std::size_t kMaxGenerated = 4;
  static constexpr std::size_t kMaxRawLength = 64;
  static constexpr std::uint64_t kActivationSupport = 6;
  static constexpr std::size_t kActivationVariants = 3;

  RuleStore() = default;

  // Observe one omission, extra letter, substitution or adjacent transposition.
  // Ambiguous edits and edits requiring multiple operations are rejected.  Inputs are lowercase ASCII pinyin only.
  bool observe(std::string_view raw, std::string_view corrected);

  // Merge a serialized rule. Invalid records and records beyond the bounded
  // capacity are ignored. Existing evidence is merged and never overwritten.
  void absorb(const Rule& rule);

  // Generate corrections for an unseen raw pinyin.  Only active rules apply;
  // at most four unique corrected pinyins are returned.
  std::vector<std::string> generate(std::string_view raw,
                                    bool include_observed = false) const;

  // Beta-smoothed evidence share among competing edits in the same input
  // context. No model inference may promote its own output as new evidence.
  double confidence(const Rule &) const noexcept;
  static bool valid_rule(const Rule&) noexcept;

  const std::vector<Rule>& rules() const noexcept { return rules_; }
  std::vector<Rule> snapshot() const { return rules_; }
  std::size_t size() const noexcept { return rules_.size(); }
  void clear() noexcept { rules_.clear(); }

 private:
  static bool valid_raw(std::string_view value) noexcept;
  static bool same_key(const Rule& a, const Rule& b) noexcept;
  static bool contains(const std::vector<std::string>& values,
                       std::string_view value) noexcept;
  static void add_bounded(std::vector<std::string>& values,
                          std::string_view value, std::size_t limit);

  std::vector<Rule> rules_;
};

// A descriptive alias for callers that prefer model terminology.
using CorrectionModel = RuleStore;

}  // namespace tipe::correction
