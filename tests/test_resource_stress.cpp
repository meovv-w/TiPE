#include "tipe/learning.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/resource.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {
long env_long(const char *name, long fallback, long low, long high) {
  const char *raw = std::getenv(name);
  if (!raw || !*raw) return fallback;
  try {
    const long value = std::stol(raw);
    return std::clamp(value, low, high);
  } catch (...) {
    return fallback;
  }
}

uint64_t directory_bytes(const fs::path &dir) {
  uint64_t total = 0;
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (ec) break;
    std::error_code item_ec;
    if (!entry.is_regular_file(item_ec)) continue;
    const auto size = entry.file_size(item_ec);
    if (item_ec) continue;
    total += static_cast<uint64_t>(size);
  }
  return total;
}

// Linux exposes the resident high-water mark through getrusage.  Reading
// VmRSS as well lets the test observe growth while the worker is running,
// rather than only after all batches have drained.
uint64_t current_rss_kb() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (!line.starts_with("VmRSS:")) continue;
    std::istringstream fields(line.substr(6));
    uint64_t value = 0;
    fields >> value;
    return value;
  }
  return 0;
}

struct Metrics {
  size_t max_queue{};
  size_t max_model{};
  uint64_t max_disk{};
  uint64_t max_rss{};
};

struct TempDirectory {
  fs::path path;
  ~TempDirectory() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
}

int run() {
  // Defaults are short enough for CTest.  The companion script sets a
  // longer duration/round count for a sustained manual run.
  const long rounds = env_long("TIPE_RESOURCE_STRESS_ROUNDS", 120, 1, 200000);
  const long events_per_round = env_long("TIPE_RESOURCE_STRESS_EVENTS", 128, 1, 4096);
  const long pause_ms = env_long("TIPE_RESOURCE_STRESS_PAUSE_MS", 10, 0, 1000);
  const bool trained = env_long("TIPE_RESOURCE_STRESS_TRAINED",0,0,1);
  const fs::path dir = fs::path("/tmp") /
      ("tipe-resource-stress-" + std::to_string(static_cast<long>(::getpid())));
  fs::remove_all(dir);
  TempDirectory cleanup{dir};

  tipe::Learner learner(dir.string());
  Metrics metrics;
  const auto wall_start = std::chrono::steady_clock::now();
  rusage usage_before{};
  ::getrusage(RUSAGE_SELF, &usage_before);
  size_t serial = 0;

  for (long round = 0; round < rounds; ++round) {
    for (long n = 0; n < events_per_round; ++n) {
      tipe::Event event;
      event.type = tipe::EventType::Commit;
      const size_t sample = trained ? serial / 3 : serial;
      ++serial;
      event.pinyin = "sustained" + std::to_string(sample);
      event.committed = trained ? "词" + std::to_string(sample) : "词";
      event.context = (round % 3 == 0) ? "editor" :
                      (round % 3 == 1) ? "terminal" : "browser";
      learner.push(std::move(event));
    }
    metrics.max_queue = std::max(metrics.max_queue, learner.queue_depth());
    metrics.max_model = std::max(metrics.max_model, learner.model()->entries.size());
    metrics.max_disk = std::max(metrics.max_disk, learner.disk_bytes());
    metrics.max_rss = std::max(metrics.max_rss, current_rss_kb());
    if (pause_ms) std::this_thread::sleep_for(std::chrono::milliseconds(pause_ms));
  }

  // A bounded drain is part of the contract: resource assertions are made
  // only after all accepted batches had a chance to publish.
  require(learner.wait_until_idle(20s), "worker did not drain within 20 seconds");
  if (trained)
    require(learner.model()->language_model.transitions()>0,
            "repeated confirmations did not train the language model");
  metrics.max_queue = std::max(metrics.max_queue, learner.queue_depth());
  metrics.max_model = std::max(metrics.max_model, learner.model()->entries.size());
  metrics.max_disk = std::max(metrics.max_disk, learner.disk_bytes());
  metrics.max_rss = std::max(metrics.max_rss, current_rss_kb());

  rusage usage_after{};
  ::getrusage(RUSAGE_SELF, &usage_after);
  const auto cpu_us =
      (usage_after.ru_utime.tv_sec - usage_before.ru_utime.tv_sec) * 1000000LL +
      (usage_after.ru_utime.tv_usec - usage_before.ru_utime.tv_usec) +
      (usage_after.ru_stime.tv_sec - usage_before.ru_stime.tv_sec) * 1000000LL +
      (usage_after.ru_stime.tv_usec - usage_before.ru_stime.tv_usec);
  const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - wall_start).count();
  const uint64_t actual_disk = directory_bytes(dir);

  // These are implementation bounds, intentionally much wider than normal
  // samples so a busy CI host cannot cause a false failure. They catch an
  // unbounded queue/model/disk regression and runaway per-event allocations.
  require(metrics.max_queue <= 2048, "event queue exceeded 2048 entries");
  require(metrics.max_model <= 4096, "published model exceeded 4096 entries");
  require(actual_disk < 8ULL * 1024 * 1024, "state directory exceeded 8 MiB");
  require(learner.disk_bytes() == actual_disk, "disk_bytes differs from directory scan");
  require(metrics.max_rss > 0, "Linux VmRSS sampling is unavailable");
  require(metrics.max_rss < 256ULL * 1024, "sampled RSS exceeded 256 MiB");
  require(wall_ms >= 1, "invalid wall-clock measurement");
  // Keep a generous guard against a tight spin loop.  Normal model
  // publication can be CPU-heavy while sorting the bounded 4096-entry table,
  // and this test deliberately samples disk usage every round.  The exact
  // sample is printed below for capacity tracking; leave enough headroom for
  // slower CI hosts while still rejecting an accidental runaway worker.
  require(cpu_us < (wall_ms * 1000 + 30000000), "worker CPU exceeded regression envelope");
  // The writer exposes thread CPU separately from the process (which also
  // includes this test's directory/RSS sampling). A sustained overload must
  // remain duty-cycled instead of occupying one core continuously.
  const auto worker_cpu_us = learner.worker_cpu_us();
  require(worker_cpu_us <= static_cast<uint64_t>(wall_ms * 1000 * 3 / 4 + 100000),
          "learning worker exceeded 75% CPU duty");
  for (const char *name : {"model.current.tmp", "model.previous.tmp",
                           "state.current.tmp", "control.current.tmp"})
    require(!fs::exists(dir / name), "temporary publication file remains after drain");

  std::cout << "TiPE sustained resource stress\n"
            << "rounds=" << rounds << " events=" << serial
            << " trained=" << trained
            << " transitions=" << learner.model()->language_model.transitions()
            << " max_queue=" << metrics.max_queue
            << " max_model=" << metrics.max_model
            << " max_disk=" << metrics.max_disk
            << " final_disk=" << actual_disk
            << " max_sampled_rss_kb=" << metrics.max_rss
            << " process_peak_rss_kb=" << usage_after.ru_maxrss
            << " cpu_ms=" << cpu_us / 1000.0
            << " worker_cpu_ms=" << worker_cpu_us / 1000.0
            << " worker_throttled_ms=" << learner.worker_throttled_us() / 1000.0
            << " wall_ms=" << wall_ms
            << " publishes=" << learner.publish_count()
            << " dropped=" << learner.dropped_events() << '\n';
  return 0;
}

int main() {
  try {
    return run();
  } catch (const std::exception &error) {
    std::cerr << "TiPE resource stress failed: " << error.what() << '\n';
    return 1;
  }
}
