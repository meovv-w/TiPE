#include "tipe/core.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sys/resource.h>
#include <vector>
#include <unistd.h>

int main(int argc, char **argv) {
  bool trained=false,representative=false;
  for(int i=1;i<argc;++i) {
    const std::string_view argument=argv[i];
    if(argument=="--trained-local")trained=true;
    else if(argument=="--representative")representative=true;
    else {
      std::cerr << "用法：tipe-diagnose [--trained-local] [--representative]\n";
      return 2;
    }
  }
  const std::string dir = "/tmp/tipe-diagnose-" + std::to_string(::getpid());
  struct Cleanup {
    std::string path;
    ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path,ec); }
  } cleanup{dir};
  tipe::Engine e(dir);
  e.setContext("diagnose");
  if (trained) {
    // Train through the real background publication path before sampling.
    // These variants cover lower Chinese alternatives and unseen bigrams.
    for (const auto &text : {"你好","你号","你在","海边","上海","世间","世界","时间"}) {
      for (int i=0; i<6; ++i) {
        tipe::Event event;
        event.type=tipe::EventType::CandidateSelect;
        event.context="diagnose"; event.pinyin="sample";
        event.committed=text; event.session=100+i;
        e.learner().push(event);
      }
    }
    if (!e.learner().wait_until_idle(std::chrono::seconds(10)) ||
        !e.learner().model()->language_model.transitions() ||
        !e.learner().models().select(tipe::ModelBackend::LocalSmall)) {
      std::cerr << "本地模型样本未准备完成\n";
      return 1;
    }
  }
  std::cout << "解码后端: " << e.decoderStatus() << "\n";
  std::cout << "采样模型: " << (trained ? "本地小模型（已发布字符转移）" : "TiP")
            << " 转移数=" << e.learner().model()->language_model.transitions() << "\n";
  std::vector<double> samples;
  samples.reserve(1000);
  std::vector<double> key_samples;
  key_samples.reserve(5000);
  rusage before{};
  getrusage(RUSAGE_SELF, &before);
  for (int i = 0; i < 1000; ++i) {
    e.reset();
    auto t = std::chrono::steady_clock::now();
    for (char c : std::string("nihao")) {
      auto kt = std::chrono::steady_clock::now();
      e.key(std::string(1, c));
      key_samples.push_back(std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - kt).count());
    }
    samples.push_back(std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t).count());
  }
  // Include one complete commit so the report also exercises the persistence
  // path and reports a non-zero model write estimate.
  e.key("Space");
  e.learner().wait_until_idle(std::chrono::seconds(5));
  const auto diag_observed = e.learner().observed();
  const auto diag_learned = e.learner().learned();
  rusage after{};
  getrusage(RUSAGE_SELF, &after);
  std::sort(samples.begin(), samples.end());
  std::sort(key_samples.begin(), key_samples.end());
  auto q = [&](double p) { return samples[static_cast<size_t>(p * (samples.size() - 1))]; };
  auto kq = [&](double p) { return key_samples[static_cast<size_t>(p * (key_samples.size() - 1))]; };
  const auto cpu_us = (after.ru_utime.tv_sec - before.ru_utime.tv_sec) * 1000000LL
                    + (after.ru_utime.tv_usec - before.ru_utime.tv_usec)
                    + (after.ru_stime.tv_sec - before.ru_stime.tv_sec) * 1000000LL
                    + (after.ru_stime.tv_usec - before.ru_stime.tv_usec);
  std::cout << "TiPE 诊断\n"
            << "热路径样本（每次 nihao）：p50=" << q(.50)
            << " ms p95=" << q(.95) << " ms p99=" << q(.99) << " ms\n"
            << "目标: 单键 p95 < 5 ms（整段样本另列）\n"
            << "逐键样本（5000 次）：p50=" << kq(.50)
            << " ms p95=" << kq(.95) << " ms p99=" << kq(.99) << " ms\n"
            << "进程资源：CPU=" << cpu_us / 1000.0 << " ms"
            << " 峰值RSS=" << after.ru_maxrss << " KB\n"
            << "学习计数：观察=" << e.learner().observed()
            << " 已学=" << e.learner().learned()
            << " 队列深度=" << e.learner().queue_depth()
            << " 丢弃=" << e.learner().dropped_events()
            << " 发布=" << e.learner().publish_count()
            << " 写入估计=" << e.learner().disk_bytes() << " bytes\n"
            << "学习线程：CPU=" << e.learner().worker_cpu_us() / 1000.0
            << " ms 主动让出=" << e.learner().worker_throttled_us() / 1000.0 << " ms\n"
            << "候选协议: 标准 InputPanel（由 fcitx5 负责 text-rectangle 锚定）\n"
            << "Wayland popup: GTK/Qt 客户端及网格版 classicui 使用共享绘制器；独立 tipe-ui addon 回退标准 InputPanel\n"
            << "学习队列: 有界 2048，后台低优先级，每批按 CPU 时间等长让出，默认本地\n";
  if(representative) {
    struct Case {std::string name,raw;std::vector<std::string> distinct;};
    std::string repeated;
    for(int n=0;n<12;++n)repeated+="nihao";
    std::vector<Case> cases={
      {"普通整句","nizaiganshenme"},
      {"长词","zhonghuarenmingongheguo"},
      {"长句","wojintianxiangqubeijingtiananmenguangchang"},
      {"长拼音跨解码窗口",repeated+"zhongguo"},
      {"超长组合","ni"+repeated+repeated+repeated+repeated},
      {"无效重复拼写",std::string(64,'a')},
      {"大小写英文","TypeScript"},
      {"网址","https://example.org:8080/api?page=2"}
    };
    std::vector<std::string> distinct;
    for(const auto *initial:{"ni","xi","mi","li","bi","pi","di","ti","ji","qi",
                             "yi","zi","ci","si","ri","zhi","chi","shi","nu","lu"})
      distinct.push_back(std::string(initial)+"hao"+repeated);
    cases.push_back({"不同长组合",{},std::move(distinct)});
    std::cout<<"代表性操作采样（每种20轮，包含首轮；核心线程墙钟耗时）\n";
    for(const auto &item:cases) {
      std::vector<double> typing,expansion,navigation,editing;
      auto measure=[&](std::vector<double>&values,const std::string&key){
        const auto begin=std::chrono::steady_clock::now();
        e.key(key);
        values.push_back(std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-begin).count());
      };
      for(int round=0;round<20;++round) {
        e.reset();
        const auto &raw=item.distinct.empty()?item.raw:item.distinct[round%item.distinct.size()];
        for(char ch:raw)measure(typing,std::string(1,ch));
        measure(expansion,"Down");
        for(const auto *key:{"Right","Down","Up","Left","PageDown","PageUp"})measure(navigation,key);
        e.key("ShiftTab");e.key("Home");
        measure(editing,"Delete");measure(editing,"x");measure(editing,"BackSpace");
      }
      auto report=[&](const char*operation,std::vector<double>values){
        std::sort(values.begin(),values.end());
        auto quantile=[&](double p){return values[static_cast<size_t>(p*(values.size()-1))];};
        const auto detail=item.distinct.empty()?std::to_string(item.raw.size())+"字节":std::string("每轮不同拼写");
        std::cout<<item.name<<"（"<<detail<<"） "<<operation
                 <<" 样本="<<values.size()<<" p50="<<quantile(.5)<<"ms p95="<<quantile(.95)
                 <<"ms p99="<<quantile(.99)<<"ms max="<<values.back()<<"ms\n";
      };
      report("逐键输入",typing);report("展开候选",expansion);
      report("候选导航",navigation);report("中间编辑",editing);
    }
  }
}
