#include "tipe/correction.hpp"

#include <algorithm>
#include <cassert>
#include <string>

using tipe::correction::Rule;
using tipe::correction::RuleStore;

static bool has(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

int main() {
  // Three distinct corrected spellings and six observations activate the
  // beginning-of-word n omission rule. It then generalizes to an unseen raw.
  RuleStore model;
  assert(model.observe("ihao", "nihao"));
  assert(model.observe("ihao", "nihao"));
  assert(model.observe("imen", "nimen"));
  assert(model.observe("imen", "nimen"));
  assert(model.observe("izao", "nizao"));
  assert(model.observe("izao", "nizao"));
  assert(model.size() == 1);
  assert(model.rules().front().support == 6);
  assert(model.rules().front().corrected_pinyin.size() == 3);
  assert(model.rules().front().active());
  assert(has(model.generate("izai"), "nizai"));
  // An already observed raw spelling is never proposed by its own rule.
  assert(model.generate("ihao").empty());
  // The neighboring letters are part of the key; the i rule cannot migrate
  // to another initial letter.
  assert(model.generate("azai").empty());

  // Fewer than six supports, even with three variants, does not activate.
  RuleStore threshold;
  assert(threshold.observe("iha", "niha"));
  assert(threshold.observe("ime", "nime"));
  assert(threshold.observe("izo", "nizo"));
  assert(threshold.observe("iha", "niha"));
  assert(threshold.observe("ime", "nime"));
  assert(threshold.size() == 1);
  assert(!threshold.rules().front().active());
  assert(threshold.generate("izai").empty());

  // An edit that drops or adds more than one character is rejected. Ambiguous
  // repeated-letter insertions are rejected as well.
  RuleStore malformed;
  assert(!malformed.observe("hao", "nihao"));
  assert(!malformed.observe("ihao", "nixhao"));
  assert(!malformed.observe("aa", "aaa"));
  assert(!malformed.observe("Ihao", "nihao"));

  // Merging a serialized rule clips both collections, while retaining all
  // useful bounded evidence and preserving activation thresholds.
  RuleStore clipped;
  Rule incoming;
  incoming.missing = 'n';
  incoming.left = '^';
  incoming.right = 'i';
  incoming.support = 20;
  for (int i = 0; i < 20; ++i) {
    incoming.corrected_pinyin.push_back("ni" + std::string(i + 1, 'a'));
    incoming.observed_raw.push_back("i" + std::string(i + 1, 'a'));
  }
  clipped.absorb(incoming);
  assert(clipped.size() == 1);
  assert(clipped.rules().front().corrected_pinyin.size() == 16);
  assert(clipped.rules().front().observed_raw.size() == 16);
  assert(clipped.rules().front().active());
  assert(clipped.generate("izai").size() == 1);

  // Rules have a hard global bound and never allocate a 129th key.
  RuleStore bounded;
  std::size_t attempted = 0;
  for (char left = 'a'; left <= 'z' && bounded.size() < RuleStore::kMaxRules; ++left) {
    for (char right = 'a'; right <= 'z' && bounded.size() < RuleStore::kMaxRules; ++right) {
      if (left == 'n' || right == 'n') continue;  // avoid ambiguous "n" insertion
      const std::string raw{left, right};
      const std::string corrected{left, 'n', right};
      ++attempted;
      assert(bounded.observe(raw, corrected));
    }
  }
  assert(attempted >= RuleStore::kMaxRules);
  assert(bounded.size() == RuleStore::kMaxRules);
  assert(!bounded.observe("zz", "znz"));

  // Every single-edit type uses the same evidence threshold and local
  // boundary context, and generalizes to a held-out spelling.
  struct Example { const char *raw; const char *corrected; };
  const std::vector<std::vector<Example>> edits = {
    {{"xihao","ihao"},{"ximen","imen"},{"xizao","izao"}},
    {{"vihao","nihao"},{"vimen","nimen"},{"vizao","nizao"}},
    {{"in hao","nihao"}}, // malformed whitespace never enters the model
    {{"inhao","nihao"},{"inhai","nihai"},{"inhua","nihua"}}
  };
  for (size_t j : {size_t(0),size_t(1),size_t(3)}) {
    RuleStore edits_model;
    for (const auto &ex : edits[j]) for (int n=0;n<2;++n)
      assert(edits_model.observe(ex.raw,ex.corrected));
    assert(edits_model.size()==1 && edits_model.rules()[0].active());
    const std::string held_raw = j==0 ? "xizai" : j==1 ? "vizai" : "inhou";
    const std::string held_corrected = j==0 ? "izai" : j==1 ? "nizai" : "nihou";
    assert(has(edits_model.generate(held_raw),held_corrected));
    assert(edits_model.generate("unrelated").empty());
    RuleStore restored;
    for (const auto &rule : edits_model.snapshot()) restored.absorb(rule);
    assert(restored.generate(held_raw)==edits_model.generate(held_raw));
  }
  assert(!malformed.observe("in hao","nihao"));
  assert(!malformed.observe("nihao","nihao"));
  assert(!malformed.observe("nnihao","nihao")); // ambiguous extra n
  assert(!malformed.observe("vihzo","nihao")); // two substitutions

  // Conflicting corrections in the same input context lower confidence.
  // Arrival order cannot decide which letter gets inserted automatically.
  RuleStore conflict;
  for (const auto &variant : {"hao","men","zao"}) {
    for (int n=0;n<2;++n) {
      assert(conflict.observe(std::string("i")+variant,std::string("ni")+variant));
      assert(conflict.observe(std::string("i")+variant,std::string("li")+variant));
    }
  }
  assert(conflict.rules().size()==2);
  assert(conflict.confidence(conflict.rules()[0]) < .8);
  assert(conflict.generate("izai").empty());
  for (int n=0;n<30;++n) assert(conflict.observe("ihao","nihao"));
  assert(has(conflict.generate("izai"),"nizai"));
  assert(!has(conflict.generate("izai"),"lizai"));
  return 0;
}
