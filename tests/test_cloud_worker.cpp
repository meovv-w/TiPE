#include "tipe/learning.hpp"
#include "tipe/core.hpp"

#include <cassert>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

namespace {
struct Server {
  int fd{-1}; uint16_t port{}; std::thread thread; std::string request;
  explicit Server(std::string response_body = "{}") {
    fd = ::socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    int reuse = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address); assert(::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    port = ntohs(address.sin_port); assert(::listen(fd, 1) == 0);
    thread = std::thread([this, body = std::move(response_body)] {
      const int client = ::accept(fd, nullptr, nullptr); if (client < 0) return;
      char buffer[2048];
      while (true) { const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0); if (n <= 0) break;
        request.append(buffer, static_cast<size_t>(n)); if (request.find("\r\n\r\n") != std::string::npos) break; }
      const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL); ::close(client);
    });
  }
  ~Server() {
    if (fd >= 0) { const int wake = ::socket(AF_INET, SOCK_STREAM, 0); if (wake >= 0) {
      sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
      (void)::connect(wake, reinterpret_cast<sockaddr *>(&address), sizeof(address)); ::close(wake); }
      ::close(fd); fd = -1; }
    if (thread.joinable()) thread.join();
  }
};
}

int main() {
  tipe::CloudWorker worker;
  tipe::CloudTransportOptions options;
  options.enabled = true;
  options.timeout = std::chrono::milliseconds(500);
  Server server;
  options.endpoint = "http://127.0.0.1:" + std::to_string(server.port) + "/enhance";
  worker.configure(options, true);

  tipe::Event event; event.type = tipe::EventType::Commit; event.pinyin = "nihao";
  event.committed = "你好"; event.context = "/private/editor"; event.preedit = "RAW_PREEDIT";
  event.key = "RAW_KEY";
  const auto start = std::chrono::steady_clock::now();
  worker.enqueue(event);
  const auto enqueue_time = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  assert(enqueue_time.count() < 20);
  assert(worker.wait_until_idle(std::chrono::seconds(2)));
  const auto status = worker.status();
  assert(status.sent == 1 && status.failed == 0);
  assert(server.request.find("RAW_KEY") == std::string::npos);
  assert(server.request.find("RAW_PREEDIT") == std::string::npos);
  assert(server.request.find("context_token") != std::string::npos);

  worker.pause(true);
  worker.enqueue(event);
  assert(worker.status().queued == 0);
  worker.pause(false);
  worker.cancel();
  assert(worker.wait_until_idle(std::chrono::milliseconds(200)));

  // Learner wires the same scheduler only when all explicit environment
  // gates are present. This proves the input path merely enqueues work; the
  // HTTP exchange is still performed by the worker thread.
  Server learner_server(R"({"candidates":[{"pinyin":"nihao","text":"云端备选","score":900}]})");
  const std::string state_dir = "/tmp/tipe-cloud-worker-learner-" + std::to_string(::getpid());
  ::setenv("TIPE_CLOUD_ENABLED", "1", 1);
  ::setenv("TIPE_CLOUD_CONSENT", "1", 1);
  ::setenv("TIPE_CLOUD_ENDPOINT", ("http://127.0.0.1:" + std::to_string(learner_server.port) + "/enhance").c_str(), 1);
  {
    tipe::Engine engine(state_dir);
    auto &learner = engine.learner();
    engine.setContext(event.context);
    assert(learner.models().select(tipe::ModelBackend::Cloud));
    learner.push(event);
    for (int i = 0; i < 80 && learner.cloud_status().sent == 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    assert(learner.cloud_status().sent == 1);
    auto remote = learner.cloud_candidates("nihao", event.context);
    assert(remote.size() == 1 && remote.front().text == "云端备选");
    tipe::View view;
    for (char ch : std::string("nihao")) view = engine.key(std::string(1, ch));
    assert(!view.candidates.empty() && view.candidates.front().text == "你好");
    assert(view.candidates.back().text == "云端备选");
    assert(learner.cloud_candidates("nihao", "another-editor").empty());
    assert(learner.models().select(tipe::ModelBackend::TiP));
    engine.reset();
    for (char ch : std::string("nihao")) view = engine.key(std::string(1, ch));
    assert(std::none_of(view.candidates.begin(), view.candidates.end(), [](const auto &c) { return c.text == "云端备选"; }));
  }
  ::unsetenv("TIPE_CLOUD_ENABLED"); ::unsetenv("TIPE_CLOUD_CONSENT"); ::unsetenv("TIPE_CLOUD_ENDPOINT");
  std::filesystem::remove_all(state_dir);

  // A refused endpoint is retried with bounded backoff and then dropped;
  // the worker never grows an unbounded retry queue.
  tipe::CloudWorker retry_worker;
  tipe::CloudTransportOptions retry_options;
  retry_options.enabled = true; retry_options.endpoint = "http://127.0.0.1:1/";
  retry_options.timeout = std::chrono::milliseconds(80);
  retry_worker.configure(retry_options, true);
  retry_worker.enqueue(event);
  for (int i = 0; i < 160 && retry_worker.status().failed == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  const auto retry_status = retry_worker.status();
  assert(retry_status.failed == 1 && retry_status.retries == 3 && retry_status.queued == 0);
  return 0;
}
