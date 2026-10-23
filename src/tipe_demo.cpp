#include "tipe/core.hpp"
#include <iostream>
#include <string_view>

namespace {
std::string json(std::string_view text) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c < 32) {
      out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
    } else out += c;
  }
  return out + '"';
}
}

int main(int argc, char **argv) {
  bool json_lines = false;
  std::string state_dir;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option = argv[i];
    if (option == "--json-lines") json_lines = true;
    else if (option == "--state-dir" && i + 1 < argc) state_dir = argv[++i];
    else {
      std::cerr << "用法: tipe-demo [--json-lines] [--state-dir 目录]\n";
      return option == "--help" ? 0 : 2;
    }
  }
  tipe::Engine engine(state_dir);
  if (!json_lines)
    std::cout << "TiPE demo; 输入字母、Space、数字、Up/Down/Left/Right、Shift，Ctrl-D退出\n";
  std::string key;
  while (std::cin >> key) {
    if (key == "Ctrl-D") break;
    const auto view = engine.key(key);
    if (json_lines) {
      std::cout << "{\"key\":" << json(key)
                << ",\"mode\":" << json(view.mode == tipe::Mode::Chinese ? "Chinese" : "English")
                << ",\"preedit\":" << json(view.preedit)
                << ",\"preedit_cursor\":" << view.preedit_cursor
                << ",\"commit\":" << json(view.commit)
                << ",\"expanded\":" << (view.expanded ? "true" : "false")
                << ",\"cursor\":" << view.cursor << ",\"candidates\":[";
      for (size_t i = 0; i < view.candidates.size(); ++i) {
        if (i) std::cout << ',';
        const auto &candidate = view.candidates[i];
        std::cout << "{\"text\":" << json(candidate.text)
                  << ",\"pinyin\":" << json(candidate.pinyin)
                  << ",\"consumed\":" << candidate.consumed << '}';
      }
      std::cout << "]}\n";
    } else {
      std::cout << "mode=" << (view.mode == tipe::Mode::Chinese ? "中文" : "英文")
                << " preedit=" << view.preedit
                << (view.commit.empty() ? "" : " commit=" + view.commit)
                << (view.expanded ? " [展开]" : "") << " candidates:";
      for (size_t i = 0; i < view.candidates.size(); ++i)
        std::cout << ' ' << i + 1 << ':' << view.candidates[i].text;
      std::cout << '\n';
    }
  }
}
