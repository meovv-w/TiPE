#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tipe {
// Bounded character bigram model. Training runs only while constructing a
// background snapshot. Readers perform immutable, bounded lookups. No raw
// keyboard events or surrounding text are part of the model.
class LocalLanguageModel {
 public:
  void observe(std::string_view text, std::string_view context, uint64_t support);
  double score(std::string_view text, std::string_view context) const;
  size_t transitions() const { return transitions_; }
 private:
  struct Context {
    std::unordered_map<uint64_t, uint32_t> pairs;
    std::unordered_map<uint32_t, uint32_t> totals;
  };
  std::unordered_map<std::string, Context> contexts_;
  size_t transitions_{};
};
}
