#include "tipe/core.hpp"
#include "base_language_model.hpp"

#include <libime/core/userlanguagemodel.h>
#include <libime/pinyin/pinyindictionary.h>
#include <libime/pinyin/pinyincontext.h>
#include <libime/pinyin/pinyinime.h>

#include <cassert>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {
struct Row { std::string raw, expected; };

std::vector<Row> read_rows(const std::string &filename="quality.tsv") {
  std::vector<Row> rows;
  std::ifstream f(std::string(TIPE_QUALITY_DATA_DIR) + "/"+filename);
  assert(f && "quality corpus is part of the source tree");
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto tab = line.find('\t');
    assert(tab != std::string::npos);
    rows.push_back({line.substr(0, tab), line.substr(tab + 1)});
  }
  assert(!rows.empty());
  return rows;
}

std::string type(tipe::Engine &e, const std::string &raw) {
  for (char c : raw) e.key(std::string(1, c));
  auto v = e.key("Space");
  assert(v.commit.size() > 0);
  return v.commit;
}

struct BaseDecoder {
  std::unique_ptr<libime::PinyinIME> ime;
  std::unique_ptr<libime::PinyinContext> context;
  BaseDecoder() {
    auto d = std::make_unique<libime::PinyinDictionary>();
    d->load(0, "/usr/share/libime/sc.dict", libime::PinyinDictFormat::Binary);
    auto lm = std::make_unique<libime::UserLanguageModel>("/usr/lib64/libime/zh_CN.lm");
    ime = std::make_unique<libime::PinyinIME>(std::move(d), std::move(lm));
    ime->setNBest(30);
    ime->setWordCandidateLimit(15);
    context = std::make_unique<libime::PinyinContext>(ime.get());
  }
  std::string first(const std::string &raw) {
    if (context->size()) context->erase(0, context->size());
    context->type(raw);
    assert(!context->candidates().empty());
    return context->candidates().front().toString();
  }
};

void train_unrelated(tipe::Learner &l) {
  // This is deliberately larger than a normal idle batch.  None of these
  // records has the test context; the exact index must keep them out.
  for (int i = 0; i < 400; ++i) {
    tipe::Event e;
    e.type = tipe::EventType::Commit;
    e.pinyin = (i & 1) ? "qwertyuiop" : "shijian";
    e.committed = (i & 1) ? "无关词" : "时间";
    e.context = (i & 1) ? "quality-context" : "other-application";
    l.push(std::move(e));
  }
  assert(l.wait_until_idle(std::chrono::seconds(5)));
}

void empty_history_equivalence(const std::vector<Row> &rows) {
  const std::string dir="/tmp/tipe-empty-history-equivalence";
  std::filesystem::remove_all(dir);
  tipe::Engine engine(dir);engine.setContext("empty-history-equivalence");
  BaseDecoder original;
  tipe::BaseLanguageModel optimized("/usr/lib64/libime/zh_CN.lm");
  size_t score_checks=0;
  auto compare=[&] {
    auto view=engine.view();
    std::vector<tipe::Candidate> expected;
    std::vector<float> original_scores;
    for(const auto &candidate:original.context->candidates()) {
      if(candidate.sentence().empty())continue;
      // Check the actual float, not just its rounded public integer score.
      // Independent opaque states ensure each complete word chain agrees.
      auto reference_state=original.ime->model()->nullState();
      auto optimized_state=optimized.nullState();
      for(const auto *word:candidate.sentence()) {
        libime::State reference_out{},optimized_out{};
        const float reference_score=original.ime->model()->score(reference_state,*word,reference_out);
        const float optimized_score=optimized.score(optimized_state,*word,optimized_out);
        assert(reference_score==optimized_score);
        ++score_checks;reference_state=reference_out;optimized_state=optimized_out;
      }
      const auto consumed=candidate.sentence().back()->to()->index();
      if(!consumed || consumed>engine.raw().size() || consumed>64)continue;
      const auto text=candidate.toString();
      if(std::any_of(expected.begin(),expected.end(),[&](const auto &entry){
          return entry.text==text && entry.consumed==consumed;}))continue;
      expected.push_back({text,original.context->candidateFullPinyin(candidate),
                          static_cast<int>(1000*candidate.score()),consumed});
      original_scores.push_back(candidate.score());
      if(expected.size()==256)break;
    }
    assert(view.candidates.size()==expected.size());
    // Native LibIME sorts by score without a tie-breaker: two untouched
    // instances can order e.g. 你好命/你好忙 differently at exactly -17.5681915.
    // Compare permutations only inside exact native float ties; all other
    // positions, candidate identities and public scores remain strict.
    auto identity=[](const tipe::Candidate &a,const tipe::Candidate &b) {
      return std::tie(a.text,a.pinyin,a.consumed)<std::tie(b.text,b.pinyin,b.consumed);
    };
    for(size_t begin=0;begin<expected.size();) {
      size_t end=begin+1;
      while(end<expected.size() && original_scores[end]==original_scores[begin])++end;
      std::sort(expected.begin()+begin,expected.begin()+end,identity);
      std::sort(view.candidates.begin()+begin,view.candidates.begin()+end,identity);
      begin=end;
    }
    for(size_t i=0;i<expected.size();++i) {
      if(view.candidates[i].text!=expected[i].text || view.candidates[i].score!=expected[i].score)
        std::cerr<<"empty-history mismatch raw="<<engine.raw()<<" index="<<i
                 <<" actual="<<view.candidates[i].text<<":"<<view.candidates[i].score
                 <<" expected="<<expected[i].text<<":"<<expected[i].score<<"\n";
      assert(view.candidates[i].text==expected[i].text);
      assert(view.candidates[i].pinyin==expected[i].pinyin);
      assert(view.candidates[i].consumed==expected[i].consumed);
      assert(view.candidates[i].score==expected[i].score);
    }
  };
  auto corpus=rows;
  const auto isolation=read_rows("quality-isolation.tsv");
  corpus.insert(corpus.end(),isolation.begin(),isolation.end());
  std::string long_raw;
  for(int i=0;i<12;++i)long_raw+="nihao";
  corpus.push_back({long_raw,{}});
  corpus.push_back({"xi"+long_raw,{}});
  corpus.push_back({std::string(64,'a'),{}});
  for(const auto &row:corpus) {
    engine.reset();original.context->clear();
    for(char ch:row.raw) {engine.key(std::string(1,ch));original.context->type(std::string(1,ch));}
    compare();
    // Delete at the end, then put the exact letter back: both lattice
    // directions must retain the original sentence/word order and scores.
    engine.key("BackSpace");original.context->erase(row.raw.size()-1,row.raw.size());
    // Some incomplete prefixes intentionally look like English to TiPE.
    if(!engine.view().candidates.empty())compare();
    engine.key(row.raw.substr(row.raw.size()-1));
    original.context->type(row.raw.substr(row.raw.size()-1));compare();
  }
  std::cout<<"empty-history optimization matches all base candidates/scores: "<<corpus.size()
           <<" spellings, "<<score_checks<<" exact float edge scores\n";
  std::filesystem::remove_all(dir);
}

void expanded_publication_regression() {
  const std::string dir="/tmp/tipe-expanded-publication";
  std::filesystem::remove_all(dir);
  tipe::Engine engine(dir);engine.setContext("expanded-publication");
  for(char ch:std::string("nihao"))engine.key(std::string(1,ch));
  auto view=engine.key("Tab");assert(view.expanded && view.candidates.front().text=="你好");
  for(int n=0;n<6;++n) {
    tipe::Event event;event.type=tipe::EventType::CandidateSelect;
    event.before=event.pinyin="nihao";event.committed="你号";
    event.context="expanded-publication";event.session=900+n;
    engine.learner().push(std::move(event));
  }
  assert(engine.learner().wait_until_idle(std::chrono::seconds(5)));
  view=engine.key("Down");
  assert(view.preedit=="nihao" && view.candidates.front().text=="你号");
  assert(engine.learner().clear());
  view=engine.key("Up");
  assert(view.preedit=="nihao" && view.candidates.front().text=="你好");
  // Repeated expansion and refreshed navigation cannot duplicate a prefix.
  engine.key("Tab");view=engine.key("Right");
  assert(std::count_if(view.candidates.begin(),view.candidates.end(),[](const auto &candidate) {
    return candidate.text=="你" && candidate.consumed==2;
  })==1);
  assert(engine.key("Enter").commit=="nihao");
}
} // namespace

int main() {
  const auto rows = read_rows();
  empty_history_equivalence(rows);
  expanded_publication_regression();
  BaseDecoder baseline;
  const std::string dir = "/tmp/tipe-quality-eval";
  std::filesystem::remove_all(dir);

  // The untouched Engine must expose the same first candidate as the direct
  // LibIME decoder for every fixed corpus row.
  {
    tipe::Engine e(dir);
    e.learner().clear();
    e.setContext("quality-context");
    for (const auto &row : rows) {
      const auto expected = baseline.first(row.raw);
      if (expected != row.expected) {
        std::cerr << "baseline mismatch raw=" << row.raw << " file=" << row.expected
                  << " actual=" << expected << "\n";
        return 2;
      }
      e.reset();
      for (char c : row.raw) e.key(std::string(1, c));
      auto view = e.key("Down"); // exercise the normal candidate refresh path
      assert(!view.candidates.empty());
      assert(view.candidates.front().text == expected);
    }

    // Unrelated pinyin and another application context must not perturb the
    // first candidate of any fixed sentence.
    train_unrelated(e.learner());
    for (const auto &row : rows) {
      e.reset();
      for (char c : row.raw) e.key(std::string(1, c));
      auto view = e.key("Down");
      assert(!view.candidates.empty() && view.candidates.front().text == row.expected);
    }
  }

  // Select a non-first real LibIME candidate three times.  The exact personal
  // entry is then allowed to outrank the base decoder and must survive restart.
  const auto &learn_row = rows.front();
  const std::string learned_text = "你号";
  {
    tipe::Engine e(dir);
    e.setContext("quality-context");
    e.learner().clear();
    for (int round = 0; round < 3; ++round) {
      e.reset();
      for (char c : learn_row.raw) e.key(std::string(1, c));
      auto view = e.key("Down");
      size_t chosen = view.candidates.size();
      for (size_t i = 1; i < view.candidates.size(); ++i)
        if (view.candidates[i].text == learned_text &&
            view.candidates[i].consumed == learn_row.raw.size()) { chosen = i; break; }
      assert(chosen < view.candidates.size());
      auto selected = e.select(chosen);
      assert(selected.commit == learned_text && selected.preedit.empty());
      assert(e.learner().wait_until_idle(std::chrono::seconds(5)));
    }
    e.reset();
    for (char c : learn_row.raw) e.key(std::string(1, c));
    auto learned = e.key("Down");
    assert(!learned.candidates.empty() && learned.candidates.front().text == learned_text);
    // A learned exact spelling must not reorder unrelated complete sentences
    // in the same application context.
    for (const auto &row : rows) {
      if (row.raw == learn_row.raw) continue;
      e.reset();
      for (char c : row.raw) e.key(std::string(1, c));
      auto sentence = e.key("Down");
      assert(!sentence.candidates.empty() && sentence.candidates.front().text == row.expected);
    }
  }
  {
    // A fresh Engine reads the atomically published model and retains the
    // exact preference without sharing the old in-memory object.
    tipe::Engine restarted(dir);
    restarted.setContext("quality-context");
    restarted.reset();
    for (char c : learn_row.raw) restarted.key(std::string(1, c));
    auto learned = restarted.key("Down");
    assert(!learned.candidates.empty() && learned.candidates.front().text == learned_text);
    assert(restarted.learner().exact_entries(learn_row.raw, "quality-context").front().text == learned_text);
  }
  std::cout << "quality evaluation passed (" << rows.size() << " corpus rows)\n";
}
