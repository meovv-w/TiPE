#include "tipe/core.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
long rss_kb() {
  std::ifstream file("/proc/self/status");std::string line;
  while(std::getline(file,line))if(line.starts_with("VmRSS:"))return std::stol(line.substr(6));
  return 0;
}
}

int main() {
  namespace fs=std::filesystem;
  const auto root=fs::path("/tmp")/("tipe-decoder-resources-"+std::to_string(getpid()));
  fs::create_directories(root);
  struct Cleanup {fs::path path;~Cleanup(){fs::remove_all(path);}} cleanup{root};
  const auto data=root/"runtime";fs::create_directory(data);
  fs::create_symlink("/usr/share/libime/sc.dict",data/"sc.dict");
  fs::create_symlink("/usr/lib64/libime/zh_CN.lm",data/"zh_CN.lm");
  setenv("TIPE_LIBIME_DATA_DIR",data.c_str(),1);
  tipe::Engine::prepareDecoder();
  const auto before=rss_kb();
  std::vector<std::unique_ptr<tipe::Engine>> contexts;
  const auto begin=std::chrono::steady_clock::now();
  for(int i=0;i<12;++i) {
    contexts.push_back(std::make_unique<tipe::Engine>((root/("state-"+std::to_string(i))).string()));
    auto &engine=*contexts.back();assert(engine.decoderBackend()==tipe::DecoderBackend::LibIME);
    engine.setContext("context-"+std::to_string(i));
    const std::string raw=i%2?"shanghai":"nihao";
    for(char ch:raw)engine.key(std::string(1,ch));
  }
  const auto after=rss_kb();
  // Each lattice and learner is independent, while a 30 MB base dictionary
  // must not be duplicated per field. Allow ample allocator/thread slack.
  assert(before>0 && after>0 && after-before<16*1024);
  for(int i=0;i<12;++i) {
    assert(contexts[i]->raw()==(i%2?"shanghai":"nihao"));
    assert(contexts[i]->key("Space").commit==(i%2?"上海":"你好"));
  }
  // A cached success cannot mask missing/replaced files at the same paths.
  fs::remove(data/"sc.dict");
  {tipe::Engine missing((root/"missing").string());
   assert(missing.decoderBackend()==tipe::DecoderBackend::BuiltinFallback);}
  std::ofstream(data/"sc.dict")<<"invalid replacement";
  {tipe::Engine corrupt((root/"corrupt").string());
   assert(corrupt.decoderBackend()==tipe::DecoderBackend::BuiltinFallback);}
  fs::remove(data/"sc.dict");
  fs::create_symlink("/usr/share/libime/sc.dict",data/"sc.dict");
  {tipe::Engine recovered((root/"recovered").string());
   assert(recovered.decoderBackend()==tipe::DecoderBackend::LibIME);}
  std::cout<<"12 independent input contexts: RSS growth="<<after-before<<" KiB, setup+typing="
           <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()
           <<" ms; replacement/fallback/recovery verified\n";
}
