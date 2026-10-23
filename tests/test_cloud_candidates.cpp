#include "tipe/learning.hpp"
#include <cassert>
#include <chrono>
#include <thread>

int main() {
  std::vector<tipe::CloudCandidate> out;
  std::string detail;
  assert(tipe::parse_cloud_candidates(
      R"({"request_id":7,"candidates":[{"pinyin":"nihao","text":"你好","score":900},{"pinyin":"nihao","text":"拟好","score":100}]})", 7, out, &detail));
  assert(out.size() == 2 && out[0].text == "你好");
  assert(tipe::parse_cloud_candidates("{}", 7, out));
  assert(out.empty());
  assert(!tipe::parse_cloud_candidates(R"({"request_id":8,"candidates":[]})", 7, out));
  assert(!tipe::parse_cloud_candidates(R"({"candidates":[{"pinyin":"nihao","text":"你好","score":1001}]})", 7, out));
  assert(!tipe::parse_cloud_candidates(R"({"candidates":[{"pinyin":"nihao","text":"你好","score":1,"extra":0}]})", 7, out));
  assert(!tipe::parse_cloud_candidates(R"({"candidates":[{"pinyin":"nihao","text":"\u202e恶","score":1}]})", 7, out));

  tipe::CloudCandidateCache cache(std::chrono::milliseconds(10), 2);
  assert(cache.put(7, "123", {{"nihao", "你好", 900}, {"nihao", "拟好", 100}}));
  assert(cache.lookup("nihao", "123").size() == 2);
  assert(cache.lookup("nihao", "456").empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(cache.lookup("nihao", "123").empty());
  assert(!cache.put(8, "bad", {{"nihao", "你好", 1}}));
  return 0;
}
