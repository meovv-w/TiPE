#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <libime/core/userlanguagemodel.h>

namespace tipe {
// TiPE never trains LibIME's history: its scoped personal model is separate.
// Preserve UserLanguageModel's empty-history mixture exactly, while avoiding
// its repeated trie probes/code extraction for every lattice edge. Calling
// LanguageModel directly without this mixture would change rare-word scores.
class BaseLanguageModel final : public libime::UserLanguageModel {
 public:
  explicit BaseLanguageModel(const char *path):libime::UserLanguageModel(path),
    history_floor_(history().unknownPenalty()),
    base_weight_(std::log10(1-historyWeight())),
    empty_history_score_(history_floor_+std::log10(historyWeight())) {}
  float score(const libime::State &state,const libime::WordNode &word,
              libime::State &out) const override {
    out={};
    const float base=libime::LanguageModel::score(state,word,out);
    // A mixture with a lower probability cannot improve the base score.
    // Use a tenfold margin so floating-point rounding near equality still
    // takes the original formula; ordinary edges need no logarithm at all.
    if(base>history_floor_+1.0F)return base;
    // KenLM uses discrete probabilities/backoffs. Different words/paths can
    // reach the same base float even within the first decode of a new input.
    // Memoize only this pure scalar calculation, with exact bit keys and a
    // fixed 2 KiB table, never candidate lists or user spellings. The model
    // itself is owned by this thread's shared decoder data.
    const auto bits=std::bit_cast<uint32_t>(base);
    auto &cached=score_cache_[(bits*2654435761U)>>24];
    if(cached.bits==bits)return cached.result;
    const float a=base+base_weight_,b=empty_history_score_;
    const float difference=std::min(a,b)-std::max(a,b);
    // Same base-10 log-sum formula as libime 1.1; underflow contributes zero.
    static const float log_10=std::log(10);
    const float addition=std::log1p(std::pow(10,difference))/log_10;
    const auto result=std::max(base,std::max(a,b)+addition);
    cached={bits,result};
    return result;
  }
 private:
  const float history_floor_,base_weight_,empty_history_score_;
  struct CachedScore {uint32_t bits{};float result{};};
  mutable std::array<CachedScore,256> score_cache_{};
};

} // namespace tipe
