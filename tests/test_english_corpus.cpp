#include "tipe/core.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <array>

namespace {
using tipe::Engine;
using tipe::View;

View type_token(Engine &e, const std::string &token) {
  View v;
  for (unsigned char c : token) v = e.key(std::string(1, static_cast<char>(c)));
  return v;
}

void expect_literal(Engine &e, const std::string &token, size_t &errors) {
  e.reset();
  const View v = type_token(e, token);
  if (v.preedit != token || !v.candidates.empty()) {
    ++errors;
    std::cerr << "literal misclassified: " << token << " preedit=" << v.preedit
              << " candidates=" << v.candidates.size() << "\n";
    return;
  }
  const View committed = e.key("Space");
  if (committed.commit != token) {
    ++errors;
    std::cerr << "literal commit mismatch: " << token << " got=" << committed.commit << "\n";
  }
}

void expect_pinyin(Engine &e, const std::string &token, size_t &errors) {
  e.reset();
  const View v = type_token(e, token);
  if (v.preedit != token || v.candidates.empty()) {
    ++errors;
    std::cerr << "pinyin misclassified: " << token << " preedit=" << v.preedit
              << " candidates=" << v.candidates.size() << "\n";
  }
}
}

int main() {
  namespace fs = std::filesystem;
  const fs::path dir = "/tmp/tipe-english-corpus";
  fs::remove_all(dir);
  Engine e(dir.string());
  e.learner().clear();
  size_t english_count = 0, code_count = 0, url_count = 0, path_count = 0,
         pinyin_count = 0, errors = 0;

  // A broad English lexicon, expanded with inflections and common compound
  // forms. The generated rows deliberately include words without a hard-coded
  // dictionary entry, so this measures the spelling heuristic rather than a
  // fixed allow-list.
  const std::array<const char *, 150> words = {
      "about","access","account","action","active","address","after","again","agent","alert",
      "allow","always","answer","anyone","apple","archive","around","arrive","article","aspect",
      "assign","audio","author","backup","balance","battery","because","before","believe","binary",
      "button","camera","cancel","change","channel","choice","circle","clean","close","cloud",
      "command","common","compile","complete","compute","connect","content","context","control","cookie",
      "copy","correct","create","current","custom","database","default","delete","design","device",
      "dialog","direct","display","download","driver","editor","email","enable","error","event",
      "example","export","feature","field","filter","finish","folder","format","forward","frame",
      "frontend","function","future","general","github","global","google","handle","header","helper",
      "history","image","import","include","index","input","install","internet","issue","keyboard",
      "language","layout","learn","library","limit","linux","local","location","machine","manager",
      "memory","message","method","minute","model","module","network","notice","object","office",
      "option","output","package","parent","parser","password","pathway","pattern","people","permission",
      "plugin","pointer","process","profile","project","prompt","python","query","reason","record",
      "release","remove","report","request","reset","result","return","review","runner","安全"};
  for (const char *word : words) {
    if (std::string(word).find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos) continue;
    expect_literal(e, word, errors); ++english_count;
    for (const char *suffix : {"ing", "ed", "s"}) {
      std::string inflected = std::string(word) + suffix;
      expect_literal(e, inflected, errors); ++english_count;
    }
  }

  // Identifiers and versioned symbols exercise uppercase, underscore, dots,
  // digits and common code naming conventions.
  const std::array<const char *, 73> identifiers = {
      "TiPECore","PinyinContext","CandidateGrid","InputPanel","QtWindow","GtkWidget","JsonParser",
      "HttpServer","TlsClient","FcitxAddon","WaylandSurface","X11Display","ModelSettings","CloudWorker",
      "LearningEvent","ResourceBudget","CommitChunk","Utf8Boundary","DecoderBackend","ControlWindow",
      "parse_json","load_config","save_state","read_model","write_snapshot","refresh_candidates","select_item",
      "candidate_grid","input_context","surrounding_text","api_client","http_server","tls_config","version_2",
      "foo.bar","foo_bar.v1","model.current","state.previous","main.cpp","core.hpp","build.ninja","CMakeLists.txt",
      "v1.2.3","sha256sum","utf8_text","x86_64","arm64_build","json_value","yaml_config","sql_query",
      "regex_match","event_loop","worker_thread","page_offset","cursor_index","commit_text","preedit_utf8",
      "libtipe.so","tipe-ui.so","libime-data","README.md","LICENSE.txt","package.json","Cargo.toml","pyproject.toml",
      "node_modules","std_vector","std_string","foo2bar","test_case_42","http2_client","api_v2","gpu0_status"};
  for (const char *token : identifiers) { expect_literal(e, token, errors); ++code_count; }

  const std::array<const char *, 71> url_hosts = {
      "example.com","example.org","github.com","gitlab.com","codeberg.org","docs.example.net","api.example.com",
      "download.example.org","packages.example.net","status.example.com","localhost.localdomain","intranet.example",
      "cdn.example.com","mirror.example.org","pypi.org","crates.io","npmjs.com","go.dev","rust-lang.org",
      "qt.io","gtk.org","wayland.app","fcitx-im.org","kernel.org","gnu.org","python.org","cplusplus.com",
      "developer.mozilla.org","wikipedia.org","openai.com","chatgpt.com","news.ycombinator.com","bugs.example.com",
      "mail.example.com","accounts.example.com","login.example.org","static.example.net","images.example.com","media.example.org",
      "files.example.net","service.example.com","v2.api.example.com","api2.example.org","foo-bar.example.com","foo_bar.example.org",
      "example.com:8080","example.org/path","example.net/a1","example.com/query?x=1","example.com/#top","www.example.com",
      "www.example.org/docs","https://example.com","http://localhost:3000","https://api.example.com/v1","https://github.com/openai",
      "https://example.org/a/b?x=1","http://127.0.0.1:8080","https://user:pass@example.com","https://cdn.example.net/file.js",
      "https://docs.example.com/search?q=tipe","https://example.com/a-b_c.2","https://www.example.org/index.html",
      "http://example.com/%E4%BD%A0%E5%A5%BD","https://example.com#section","https://example.com/?page=2","https://a.b.co",
      "www.example.net:443/path","https://localhost/health","http://example.test/api/v2","https://foo.bar/baz"};
  for (const char *token : url_hosts) { expect_literal(e, token, errors); ++url_count; }

  const std::array<const char *, 79> paths = {
      "/","/tmp","/tmp/tipe","/tmp/tipe/model.current","/home/user","/home/user/Documents","/home/user/file.txt",
      "/usr/bin/fcitx5","/usr/local/bin/tipe","/usr/lib64/libime","/usr/share/libime","/etc/hosts","/etc/fcitx5/profile",
      "/var/log/tipe.log","/var/tmp/cache","/opt/tipe/bin","/srv/http/index.html","/run/user/1000","/run/user/1000/tipe",
      "/mnt/data/input.txt","/media/user/USB","/dev/null","/proc/self/status","/sys/class/input","/lib/x86_64-linux-gnu",
      "/home/lin/Documents/ChatGPT/tipe","/home/lin/Documents/ChatGPT/tipe/build","/home/lin/Documents/ChatGPT/tipe/tests/data",
      "./relative/path","../parent/path","./build/tipe-tests","../src/tipe_core.cpp","build/tipe-tests","src/tipe_core.cpp",
      "docs/README.md","tests/test_core.cpp","data/model.bin","cache/v1/state","config/tipe.conf","logs/session.jsonl",
      "/tmp/a1","/tmp/foo-bar","/tmp/foo_bar","/tmp/v1.2","/tmp/.hidden","/tmp/tipe-123","/tmp/tipe_test_42",
      "/home/user/.config/fcitx5/profile","/home/user/.local/share/tipe/model","/home/user/.cache/tipe/index",
      "/usr/share/applications/tipe.desktop","/usr/share/icons/hicolor/scalable/apps/tipe.svg","/etc/xdg/wayland.ini",
      "/var/lib/tipe/snapshots/001","/var/lib/tipe/snapshots/current","/opt/app/releases/v2.0.1","/workspace/project/src/main.cpp",
      "/workspace/project/build/debug","/workspace/project/.git/config","/home/test/file with spaces","/tmp/path?q=1","/tmp/path#x",
      "/a/b/c/d/e","/a1/b2/c3","/foo.bar/baz","/foo_bar/baz-qux","/x86_64/bin/tool","/arm64/lib/libx.so",
      "/usr/bin/python3","/usr/bin/node","/usr/bin/env","/bin/sh","/bin/bash","/sbin/init","/root/.ssh/config",
      "/home/user/project/CMakeLists.txt","/home/user/project/package.json","/home/user/project/Cargo.toml","/home/user/project/README.md"};
  for (const char *token : paths) { expect_literal(e, token, errors); ++path_count; }

  // Common pinyin spellings and generated syllable pairs are the negative
  // control set. They must retain decoder candidates and never enter literal
  // English mode merely because the corpus above grew.
  const std::array<const char *, 50> pinyin = {
      "nihao","beijing","women","shenme","zhongguo","xianzai","keyi","shijie","shanghai","wode",
      "nimen","tamen","zhege","nage","zenme","weishenme","yinwei","suoyi","xihuan","kankan",
      "mingtian","zuotian","jintian","xianzai","yihou","gongzuo","xuexi","laoshi","xuesheng","pengyou",
      "jiating","shenghuo","chifan","shuijiao","dianhua","dianying","yinyue","tushu","xianzhi","jiandan",
      "gaoxing","kaixin","haochi","haokan","zhidao","juede","xiwang","zhichi","shuru","shuchu"};
  for (const char *token : pinyin) { expect_pinyin(e, token, errors); ++pinyin_count; }
  const std::array<const char *, 16> initials = {"ni","hao","wo","men","ta","shi","jie","zhong","guo","xian","zai","ke","yi","shui","jiao","xue"};
  for (const char *a : initials) for (const char *b : initials) {
    std::string token = std::string(a) + b;
    expect_pinyin(e, token, errors); ++pinyin_count;
  }

  std::cout << "english-corpus: english=" << english_count << " code=" << code_count
            << " urls=" << url_count << " paths=" << path_count
            << " pinyin=" << pinyin_count << " errors=" << errors << "\n";
  return errors == 0 ? 0 : 1;
}
