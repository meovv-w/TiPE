#include "tipe/core.hpp"
#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyinime.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
struct Row { std::string raw, expected; };
std::vector<Row> rows() {
  std::vector<Row> out; std::ifstream file(std::string(TIPE_QUALITY_DATA_DIR) + "/quality-isolation.tsv"); assert(file);
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto tab = line.find('\t'); assert(tab != std::string::npos);
    out.push_back({line.substr(0, tab), line.substr(tab + 1)});
  }
  assert(out.size() >= 60); return out;
}
struct Baseline {
  std::unique_ptr<libime::PinyinIME> ime; std::unique_ptr<libime::PinyinContext> context;
  Baseline() {
    auto dict = std::make_unique<libime::PinyinDictionary>();
    dict->load(0, "/usr/share/libime/sc.dict", libime::PinyinDictFormat::Binary);
    auto lm = std::make_unique<libime::UserLanguageModel>("/usr/lib64/libime/zh_CN.lm");
    ime = std::make_unique<libime::PinyinIME>(std::move(dict), std::move(lm));
    ime->setNBest(30); ime->setWordCandidateLimit(15);
    context = std::make_unique<libime::PinyinContext>(ime.get());
  }
  std::string first(const std::string &raw) {
    if (context->size()) context->erase(0, context->size());
    context->type(raw); assert(!context->candidates().empty());
    return context->candidates().front().toString();
  }
};
std::string type(tipe::Engine &engine, const std::string &raw) {
  engine.reset(); for (char c : raw) engine.key(std::string(1, c));
  auto view = engine.key("Down"); assert(!view.candidates.empty()); return view.candidates.front().text;
}
void unrelated(tipe::Learner &learner) {
  // Deliberately mix a high volume of unrelated pinyin, including one common
  // raw spelling, but keep all entries in another context. The evaluator
  // checks both context isolation and no broad global frequency pollution.
  for (int i = 0; i < 1000; ++i) {
    tipe::Event e; e.type = tipe::EventType::Commit;
    e.pinyin = (i % 3 == 0) ? "nihao" : ((i & 1) ? "qwertyuiop" : "shijian");
    e.committed = (i % 3 == 0) ? "噪声词" : ((i & 1) ? "无关词" : "时间");
    e.context = "unrelated-application"; learner.push(std::move(e));
  }
  assert(learner.wait_until_idle(std::chrono::seconds(8)));
}
} // namespace

int main() {
  const auto corpus = rows(); Baseline baseline;
  for (const auto &row : corpus) assert(baseline.first(row.raw) == row.expected);
  const std::string dir = "/tmp/tipe-quality-isolation-" + std::to_string(static_cast<long long>(::getpid()));
  std::filesystem::remove_all(dir);
  {
    tipe::Engine engine(dir); engine.setContext("quality-isolation"); engine.learner().clear();
    // The untouched engine must match direct LibIME for every category.
    for (const auto &row : corpus) assert(type(engine, row.raw) == row.expected);
    unrelated(engine.learner());
    // A noisy, differently scoped event stream cannot reorder the corpus.
    for (const auto &row : corpus) assert(type(engine, row.raw) == row.expected);

    // Three exact, user-selected alternatives are allowed to outrank only
    // their own complete spelling after repeated selection.
    struct Choice { const char *raw; const char *text; } choices[] = {
      {"nihao", "你号"}, {"zhongguo", "中过"}, {"shanghai", "伤害"}
    };
    for (const auto &choice : choices) {
      for (int round = 0; round < 3; ++round) {
        engine.reset(); for (char c : std::string(choice.raw)) engine.key(std::string(1, c));
        auto view = engine.key("Down"); size_t found = view.candidates.size();
        for (size_t i = 1; i < view.candidates.size(); ++i)
          if (view.candidates[i].text == choice.text && view.candidates[i].consumed == std::string(choice.raw).size()) { found = i; break; }
        assert(found < view.candidates.size());
        assert(engine.select(found).commit == choice.text);
        assert(engine.learner().wait_until_idle(std::chrono::seconds(5)));
      }
    }
    for (const auto &row : corpus) {
      const auto actual = type(engine, row.raw);
      if (row.raw == "nihao") assert(actual == "你号");
      else if (row.raw == "zhongguo") assert(actual == "中过");
      else if (row.raw == "shanghai") assert(actual == "伤害");
      else assert(actual == row.expected);
    }
  }
  // Reloading the published snapshot must retain only the exact choices.
  {
    tipe::Engine restarted(dir); restarted.setContext("quality-isolation");
    // The optional statistical reranker preserves the same base first tier
    // after training and restart, including exact explicit overrides.
    assert(restarted.learner().models().select(tipe::ModelBackend::LocalSmall));
    for (const auto &row : corpus) {
      const auto actual = type(restarted, row.raw);
      if (row.raw == "nihao") assert(actual == "你号");
      else if (row.raw == "zhongguo") assert(actual == "中过");
      else if (row.raw == "shanghai") assert(actual == "伤害");
      else assert(actual == row.expected);
    }
  }
  std::filesystem::remove_all(dir);
  std::cout << "quality isolation passed (" << corpus.size() << " rows; 3 exact preferences; 1000 unrelated events)\n";
}
