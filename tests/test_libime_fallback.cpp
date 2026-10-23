#include "tipe/core.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void verify_fallback(tipe::Engine &engine) {
  assert(engine.decoderBackend() == tipe::DecoderBackend::BuiltinFallback);
  assert(engine.decoderStatus().find("builtin fallback") != std::string::npos);
  for (const char c : std::string("nihao")) engine.key(std::string(1, c));
  const auto view = engine.key("Space");
  // The fallback is deliberately small, but must remain useful and safe when
  // optional package data is absent.
  assert(view.commit == "你好");
  assert(view.preedit.empty());
}
}

int main() {
  // Force an explicit, empty data scope.  The test must remain deterministic
  // on developer machines that happen to have a system LibIME installation;
  // an override is intentionally not followed by the common system paths.
  const std::filesystem::path empty = "/tmp/tipe-libime-empty-runtime";
  std::filesystem::remove_all(empty);
  std::filesystem::create_directories(empty);
  setenv("TIPE_LIBIME_DATA_DIR", empty.c_str(), 1);

  {
    tipe::Engine engine("/tmp/tipe-libime-fallback-state");
    verify_fallback(engine);
  }

  // Present but malformed data takes the exception path, which must also
  // preserve a usable engine rather than letting a C++ exception cross the
  // Fcitx addon boundary. Both files are local fixtures, so this check does
  // not depend on the host distro's language-model layout.
  std::ofstream(empty / "sc.dict", std::ios::binary) << "invalid dictionary";
  std::ofstream(empty / "zh_CN.lm", std::ios::binary) << "invalid model";
  {
    tipe::Engine engine("/tmp/tipe-libime-fallback-state");
    verify_fallback(engine);
#ifdef TIPE_HAS_LIBIME
    assert(engine.decoderStatus().find("rejected") != std::string::npos);
#endif
  }

  std::filesystem::remove_all(empty);
  std::filesystem::remove_all("/tmp/tipe-libime-fallback-state");
  std::cout << "libime runtime data missing/corrupt: bounded fallback verified\n";
}
