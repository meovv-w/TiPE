#include "tipe/core.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

using namespace std::chrono_literals;

static tipe::View type(tipe::Engine &e, const std::string &text) {
  tipe::View view;
  for (char c : text) {
    view = e.key(std::string(1, c));
    e.key(std::string(1, c), true);
  }
  return view;
}

static void correct(tipe::Engine &e, const std::string &raw,
                    const std::string &pinyin, const std::string &text) {
  e.reset();
  type(e, raw);
  for (size_t i = 0; i < raw.size(); ++i) {
    e.key("BackSpace");
    e.key("BackSpace", true);
  }
  const auto view = type(e, pinyin);
  const auto found = std::find_if(view.candidates.begin(), view.candidates.end(),
      [&](const tipe::Candidate &c) { return c.text == text && c.consumed == pinyin.size(); });
  assert(found != view.candidates.end());
  const auto committed = e.select(found - view.candidates.begin());
  assert(committed.commit == text && committed.preedit.empty());
  assert(e.learner().wait_until_idle(3s));
}

int main() {
  const std::string dir = "/tmp/tipe-correction-flow";
  std::filesystem::remove_all(dir);
  {
    tipe::Engine e(dir);
    e.setContext("correction-editor");
    auto before = type(e, "izai");
    assert(!before.candidates.empty() && before.candidates.front().text == "izai");
    e.reset();
    const auto ordinary = type(e, "nizaiganshenme").candidates.front().text;

    // Repeating one phrase is insufficient to generalize to new inputs.
    for (int i = 0; i < 6; ++i) correct(e, "ihao", "nihao", "你好");
    assert(e.learner().correction_candidates("izai", "correction-editor").empty());
    for (int i = 0; i < 2; ++i) {
      correct(e, "imen", "nimen", "你们");
      correct(e, "izao", "nizao", "你早");
    }

    // Real delete/retype/select events, rather than injected model records,
    // now produce a candidate for an unseen spelling in the same context.
    e.reset();
    auto inferred = type(e, "izai");
    assert(inferred.candidates.front().text == "你在");
    assert(inferred.candidates.front().consumed == 4);
    assert(inferred.candidates.front().inferred);
    assert(std::any_of(inferred.candidates.begin(), inferred.candidates.end(),
                      [](const auto &c) { return c.text == "izai"; }));
    const auto support = e.learner().model()->corrections.at("correction-editor").rules().front().support;
    auto committed = e.key("Space");
    assert(committed.commit == "你在" && committed.preedit.empty());
    assert(e.learner().wait_until_idle(3s));
    assert(e.learner().model()->corrections.at("correction-editor").rules().front().support == support);

    // The rule also remains useful for its original typo, while every
    // usable mature-decoder sentence keeps its original first candidate.
    e.reset();
    assert(type(e, "ihao").candidates.front().text == "你好");
    e.reset();
    assert(type(e, "nizaiganshenme").candidates.front().text == ordinary);
    e.reset();
    e.setContext("another-application");
    assert(type(e, "izai").candidates.front().text == "izai");
    e.reset();
    e.setContext("correction-editor");
    e.setSensitive(true);
    assert(type(e, "izai").candidates.front().text == "izai");
    e.setSensitive(false);
  }
  {
    tipe::Engine restarted(dir);
    restarted.setContext("correction-editor");
    const auto v = type(restarted, "izai");
    assert(v.candidates.front().text == "你在" && v.candidates.front().inferred);
    restarted.learner().clear();
    restarted.reset();
    assert(type(restarted, "izai").candidates.front().text == "izai");
  }
  // Exercise the other edit classes through the same physical-key semantic
  // path: typo, deletion, retyping, then an explicit complete Chinese choice.
  // A usable mature decode remains protected even after a rule activates.
  const auto keys_dir = dir + "-key-edits";
  std::filesystem::remove_all(keys_dir);
  struct EditCase {
    std::string context, probe, corrected;
    std::vector<std::pair<std::string, std::string>> examples;
  };
  const std::vector<EditCase> edits{
    {"extra-key", "xnizai", "nizai",
     {{"xnihao", "nihao"}, {"xnimen", "nimen"}, {"xnizao", "nizao"}}},
    {"substituted-key", "vizai", "nizai",
     {{"vihao", "nihao"}, {"vimen", "nimen"}, {"vizao", "nizao"}}},
    {"transposed-keys", "inhou", "nihou",
     {{"inhao", "nihao"}, {"inhai", "nihai"}, {"inhei", "nihei"}}}
  };
  {
    tipe::Engine e(keys_dir);
    for (const auto &edit : edits) {
      e.reset(); e.setContext(edit.context);
      const auto original = type(e, edit.probe);
      const bool protected_base = std::any_of(original.candidates.begin(), original.candidates.end(),
          [&](const auto &c) { return c.text != edit.probe && !c.pinyin.empty(); });
      for (const auto &[raw, corrected] : edit.examples) {
        e.reset();
        const auto base = type(e, corrected);
        const auto full = std::find_if(base.candidates.begin(), base.candidates.end(),
            [&](const auto &c) { return c.consumed == corrected.size() && !c.pinyin.empty(); });
        assert(full != base.candidates.end());
        const auto chosen_text = full->text;
        for (int n = 0; n < 2; ++n) correct(e, raw, corrected, chosen_text);
      }
      assert(e.learner().correction_candidates(edit.probe, edit.context) ==
             std::vector<std::string>{edit.corrected});
      assert(e.learner().correction_candidates(edit.probe, "unrelated-editor").empty());
      e.reset();
      const auto after = type(e, edit.probe);
      assert(!after.candidates.empty());
      if (protected_base) assert(after.candidates.front().text == original.candidates.front().text);
      else assert(after.candidates.front().inferred);
    }
  }
  {
    tipe::Engine restarted(keys_dir);
    for (const auto &edit : edits)
      assert(restarted.learner().correction_candidates(edit.probe, edit.context) ==
             std::vector<std::string>{edit.corrected});
    assert(restarted.learner().clear());
    for (const auto &edit : edits)
      assert(restarted.learner().correction_candidates(edit.probe, edit.context).empty());
  }
  std::filesystem::remove_all(keys_dir);
  // Publication, restart and rollback must retain each edit's operation,
  // source character and evidence. A generated choice cannot train itself.
  const auto general_dir = dir + "-general";
  std::filesystem::remove_all(general_dir);
  auto train = [](tipe::Learner &learner,const std::string &raw,const std::string &corrected) {
    tipe::Event event;
    event.type=tipe::EventType::Commit;
    event.context="general-edits";
    event.pinyin=corrected;
    event.before=corrected;
    event.committed="你";
    event.corrected_from=raw;
    learner.push(event);
  };
  {
    tipe::Learner learner(general_dir);
    for (const std::string tail : {"hao","men","zao"}) for (int n=0;n<2;++n) {
      train(learner,"xi"+tail,"i"+tail);
      train(learner,"vi"+tail,"ni"+tail);
      train(learner,"inh"+tail,"nih"+tail);
    }
    assert(learner.wait_until_idle(3s));
    assert(learner.correction_candidates("xizai","general-edits")==std::vector<std::string>{"izai"});
    assert(learner.correction_candidates("vizai","general-edits")==std::vector<std::string>{"nizai"});
    assert(learner.correction_candidates("inhou","general-edits")==std::vector<std::string>{"nihou"});
    assert(learner.export_data(general_dir+"/export.tsv"));
  }
  {
    tipe::Learner learner(general_dir);
    assert(learner.correction_candidates("xizai","general-edits")==std::vector<std::string>{"izai"});
    assert(learner.correction_candidates("vizai","general-edits")==std::vector<std::string>{"nizai"});
    assert(learner.correction_candidates("inhou","general-edits")==std::vector<std::string>{"nihou"});
    assert(learner.correction_candidates("vizai","other-context").empty());
    // Publish competing evidence; the former inference must stop applying.
    for (const std::string tail : {"hao","men","zao"}) for (int n=0;n<2;++n)
      train(learner,"vi"+tail,"li"+tail);
    assert(learner.wait_until_idle(3s));
    assert(learner.correction_candidates("vizai","general-edits").empty());
    assert(learner.rollback());
    assert(learner.correction_candidates("vizai","general-edits")==std::vector<std::string>{"nizai"});
    assert(learner.clear());
    assert(learner.correction_candidates("inhou","general-edits").empty());
  }
  std::filesystem::remove_all(general_dir);
  std::cout << "delete/retype learning for all four edits, restart and decoder isolation passed\n";
}
