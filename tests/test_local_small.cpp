#include "tipe/core.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <algorithm>

static void trained_ranking_regression() {
  const std::string dir="/tmp/tipe-trained-ranking-test";
  std::filesystem::remove_all(dir);
  {
    tipe::Engine engine(dir);
    engine.setContext("trained-ranking");
    for(char c:std::string("nihao")) engine.key(std::string(1,c));
    const auto baseline=engine.key("Tab");
    assert(baseline.candidates.size()>4);
    // Train an alternative through a different spelling. Only its character
    // probabilities can affect nihao; no exact nihao preference is available.
    const auto target=baseline.candidates[3].text;
    for(int i=0;i<6;++i) {
      tipe::Event event;
      event.type=tipe::EventType::CandidateSelect;
      event.context="trained-ranking";event.pinyin="training";
      event.committed=target;event.session=500+i;
      engine.learner().push(event);
    }
    assert(engine.learner().wait_until_idle(std::chrono::seconds(5)));
    assert(engine.learner().models().select(tipe::ModelBackend::LocalSmall));
    engine.reset();
    for(char c:std::string("nihao")) engine.key(std::string(1,c));
    const auto trained=engine.key("Tab");
    assert(trained.candidates.front().text==baseline.candidates.front().text);
    const auto found=std::find_if(trained.candidates.begin(),trained.candidates.end(),
      [&](const auto &candidate){return candidate.text==target;});
    assert(found!=trained.candidates.end());
    assert(found-trained.candidates.begin()<3);
  }
  std::filesystem::remove_all(dir);
}

static void language_model_regression() {
  tipe::LocalLanguageModel model;
  model.observe("上海", "editor", 6);
  model.observe("海边", "editor", 6);
  // Neither three-character phrase occurred in training. Ranking therefore
  // depends on learned transitions rather than an exact candidate table.
  assert(model.score("上海边","editor") > model.score("上边海","editor"));
  assert(model.score("上海边","terminal") == 0);
  const auto before=model.transitions();
  model.observe("隐私", "editor", 2);
  model.observe(std::string(300,'x'), "editor", 8);
  model.observe(std::string("ÿ",1), "editor", 8);
  assert(model.transitions()==before);
  for(int i=0;i<10000;++i)
    model.observe("学习"+std::to_string(i),"editor",3);
  assert(model.transitions()<=4096);

  const std::string state="/tmp/tipe-language-snapshot-test";
  std::filesystem::remove_all(state);
  double expected=0;
  {
    tipe::Learner learner(state);
    for(const auto &text : {"上海","海边"}) for(int i=0;i<3;++i) {
      tipe::Event event;
      event.type=tipe::EventType::CandidateSelect;
      event.pinyin="shanghai";event.committed=text;event.context="editor";
      event.session=100+i;learner.push(event);
    }
    assert(learner.wait_until_idle(std::chrono::seconds(5)));
    auto snapshot=learner.model();
    assert(snapshot->language_model.transitions()>0);
    expected=snapshot->language_model.score("上海边","editor");
    assert(expected>snapshot->language_model.score("上边海","editor"));
  }
  {
    tipe::Learner restarted(state);
    assert(restarted.model()->language_model.score("上海边","editor")==expected);
    assert(restarted.clear());
    assert(restarted.model()->language_model.transitions()==0);
  }
  std::filesystem::remove_all(state);
}

int main() {
  language_model_regression();
  trained_ranking_regression();
  const std::string dir = "/tmp/tipe-local-small-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  // A user supplied row is loaded once and used only after explicit backend
  // selection.  The ordinary TiP path remains unchanged before selection.
  {
    std::ofstream model(dir + "/local-small.tsv");
    model << "nihao\t自定义词\t999\n";
  }
  tipe::Engine engine(dir);
  for (char c : std::string("nihao")) engine.key(std::string(1, c));
  auto tip = engine.key("Tab");
  assert(!tip.candidates.empty());
  assert(tip.candidates.front().text != "自定义词");
  engine.reset();
  assert(engine.learner().models().select(tipe::ModelBackend::LocalSmall));
  for (char c : std::string("nihao")) engine.key(std::string(1, c));
  auto local = engine.key("Tab");
  assert(!local.candidates.empty());
  assert(local.candidates.front().text == "自定义词");
  auto committed = engine.key("Space");
  assert(committed.commit == "自定义词");
  assert(engine.learner().models().selected() == tipe::ModelBackend::LocalSmall);
  return 0;
}
