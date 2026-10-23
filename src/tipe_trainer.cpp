#include "tipe/learning.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {
volatile std::sig_atomic_t interrupted = 0;

void on_signal(int) { interrupted = 1; }

bool valid_pinyin(std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || c == '\'';
  });
}

bool valid_field(std::string_view value, size_t max_bytes) {
  return !value.empty() && value.size() <= max_bytes &&
         value.find_first_of("\t\r\n") == std::string_view::npos;
}

struct Options {
  std::string state_dir;
  std::string input;
  bool rollback_on_cancel{};
};

bool parse_args(int argc, char **argv, Options &out) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    auto value = [&](std::string &target) {
      if (i + 1 >= argc) return false;
      target = argv[++i];
      return !target.empty();
    };
    if (arg == "--state-dir") {
      if (!value(out.state_dir)) return false;
    } else if (arg == "--input") {
      if (!value(out.input)) return false;
    } else if (arg == "--rollback-on-cancel") {
      out.rollback_on_cancel = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "用法：tipe-trainer --state-dir DIR --input FILE [--rollback-on-cancel]\n"
                   "输入格式：拼音<TAB>词<TAB>上下文（每行一个明确选择）\n";
      return false;
    } else {
      return false;
    }
  }
  return !out.state_dir.empty() && !out.input.empty();
}
}  // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse_args(argc, argv, options)) {
    std::cerr << "需要 --state-dir DIR 和 --input FILE；使用 --help 查看格式\n";
    return 2;
  }
  std::ifstream input(options.input);
  if (!input) {
    std::cerr << "无法读取训练输入文件\n";
    return 3;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // Learner owns the bounded queue, low-priority worker and atomic model
  // publication. This executable only turns an explicit, local TSV batch into
  // semantic CandidateSelect events; it never starts a second model writer.
  tipe::Learner learner(options.state_dir);
  constexpr size_t kMaxRows = 4096;
  constexpr size_t kMaxLine = 2048;
  size_t accepted = 0, rejected = 0;
  std::string line;
  while (accepted + rejected < kMaxRows && std::getline(input, line)) {
    if (interrupted) break;
    if (line.size() > kMaxLine) {
      ++rejected;
      continue;
    }
    if (line.empty() || line.front() == '#') continue;
    const size_t first = line.find('\t');
    const size_t second = first == std::string::npos
                              ? std::string::npos
                              : line.find('\t', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        line.find('\t', second + 1) != std::string::npos) {
      ++rejected;
      continue;
    }
    const std::string pinyin = line.substr(0, first);
    const std::string text = line.substr(first + 1, second - first - 1);
    const std::string context = line.substr(second + 1);
    if (!valid_pinyin(pinyin) || !valid_field(text, 512) ||
        context.size() > 128 || context.find_first_of("\r\n\t") != std::string::npos) {
      ++rejected;
      continue;
    }
    tipe::Event event;
    event.type = tipe::EventType::CandidateSelect;
    event.key = "trainer";
    event.preedit = pinyin;
    event.pinyin = pinyin;
    event.committed = text;
    event.before = pinyin;
    event.context = context;
    event.retained = text;
    event.ts_ms = 1 + accepted;
    learner.push(std::move(event));
    ++accepted;
  }

  if (interrupted) {
    learner.pause(true);
    if (options.rollback_on_cancel) learner.rollback();
    std::cout << "已暂停：已提交 " << accepted << " 条，拒绝 " << rejected << " 条\n";
    return 130;
  }
  if (!learner.wait_until_idle(std::chrono::seconds(30))) {
    std::cerr << "后台整理超时，输入法模型保持可用；下次可自动重试\n";
    return 4;
  }
  std::cout << "TiP 后台整理完成：已提交 " << accepted << " 条，拒绝 " << rejected
            << " 条，模型版本 " << learner.status_snapshot().version << "\n";
  return 0;
}
