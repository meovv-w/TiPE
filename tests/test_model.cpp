#include "tipe/learning.hpp"

#include <cassert>
#include <limits>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>

using tipe::ModelBackend;

int main() {
  const std::string dir = "/tmp/tipe-model-test";
  std::filesystem::remove_all(dir);
  {
    tipe::ModelSettings settings(dir);
    assert(settings.selected() == ModelBackend::TiP);
    const auto list = settings.backends();
    assert(list.size() == 3);
    assert(list[0].backend == ModelBackend::TiP && list[0].available && list[0].selected);
    assert(list[1].backend == ModelBackend::LocalSmall && list[1].available);
    assert(list[2].backend == ModelBackend::Cloud && !list[2].available);
    assert(!settings.cloud_enabled());

    // LocalSmall is an opt-in, local-only backend.  Selecting it persists the
    // choice, while TiP remains the default for a fresh directory.
    assert(settings.select(ModelBackend::LocalSmall));
    assert(settings.selected() == ModelBackend::LocalSmall);
    const auto local = settings.local_small_candidates("nihao", "");
    assert(local.empty()); // no fake model made from built-in example words
    assert(!settings.select(ModelBackend::Cloud));
    assert(settings.selected() == ModelBackend::LocalSmall);
    assert(settings.set_cloud_api_key_path("/run/user/1000/tipe-api-key"));
    assert(settings.cloud_api_key_path() == "/run/user/1000/tipe-api-key");
    assert(!std::filesystem::exists(dir + "/models.current.tmp"));
    struct stat st{};
    assert(::stat((dir + "/models.current").c_str(), &st) == 0);
    assert((st.st_mode & 0777) == 0600);
    assert(settings.select(ModelBackend::TiP));
  }
  {
    tipe::ModelSettings reloaded(dir);
    assert(reloaded.selected() == ModelBackend::TiP);
    assert(!reloaded.cloud_enabled());
    assert(reloaded.cloud_api_key_path() == "/run/user/1000/tipe-api-key");
    // A hand-edited config cannot falsely activate an unavailable backend.
    {
      std::ofstream f(dir + "/models.current", std::ios::trunc);
      f << "#tipe-model-v1\tcloud\t1\tsecret\n";
    }
    assert(!reloaded.refresh());
    assert(reloaded.selected() == ModelBackend::TiP);
    assert(!reloaded.cloud_enabled());
    assert(reloaded.cloud_api_key_path() == "secret");
    std::ifstream recovered(dir + "/models.current");
    std::string line;
    assert(std::getline(recovered, line));
    assert(line.rfind("#tipe-model-v1\ttip\t0\tsecret", 0) == 0);
  }
  {
    // Learner exposes the same settings object used by the control center.
    setenv("TIPE_LEARNER_READ_ONLY", "1", 1);
    tipe::Learner learner(dir);
    assert(learner.models().selected() == ModelBackend::TiP);
  assert(!learner.models().select(ModelBackend::Cloud));

  // The cloud contract is a consent-gated, semantic-event-only preflight;
  // sensitive/key events and raw context are never serialized for transport.
  tipe::Event commit;
  commit.type = tipe::EventType::Commit;
  commit.pinyin = "nihao";
  commit.committed = "你好";
  commit.context = "/home/user/private-editor";
  commit.interval_ms = 12;
  auto denied = tipe::make_cloud_payload(commit, false, 7);
  assert(!denied);
  auto payload = tipe::make_cloud_payload(commit, true, 7);
  assert(payload && payload->pinyin == "nihao" && payload->committed == "你好");
  assert(payload->request_id == 7 && payload->context_token != commit.context);
  assert(payload->context_token.find("/") == std::string::npos);
  commit.sensitive = true;
  assert(!tipe::make_cloud_payload(commit, true, 8));
  commit.sensitive = false;
  commit.type = tipe::EventType::KeyDown;
  assert(!tipe::make_cloud_payload(commit, true, 9));
  tipe::CloudCancellation cancellation;
  assert(!cancellation.cancelled());
  cancellation.cancel();
  assert(cancellation.cancelled());
  cancellation.reset();
  assert(!cancellation.cancelled());
  }
  return 0;
}
