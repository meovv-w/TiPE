#include "tipe/learning.hpp"
#include "cloud_validation.hpp"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace tipe {
namespace {
using Clock = std::chrono::steady_clock;

CloudTransportResult failure(CloudTransportStatus status, uint64_t id, std::string detail) {
  return {status, 0, id, {}, std::move(detail)};
}

bool cancelled(const CloudCancellation *token) { return token && token->cancelled(); }

// OpenSSL's socket BIO may use write(2), which can raise SIGPIPE. Suppress it
// only for the calling thread and restore the caller's signal mask exactly.
struct SigpipeGuard {
  sigset_t blocked{}, previous{};
  bool restore{}, already_pending{};
  SigpipeGuard() {
    sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    sigset_t pending{}; sigpending(&pending); already_pending = sigismember(&pending, SIGPIPE) == 1;
    restore = pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0;
  }
  ~SigpipeGuard() {
    if (!restore) return;
    if (!already_pending && sigismember(&previous, SIGPIPE) != 1) {
      timespec zero{};
      while (sigtimedwait(&blocked, nullptr, &zero) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  }
};

struct Url { std::string host, port, target; bool tls{}; };
bool parse_url(std::string_view input, Url &out) {
  if (input.starts_with("https://")) { out.tls = true; input.remove_prefix(8); }
  else if (input.starts_with("http://")) input.remove_prefix(7);
  else return false;
  const size_t slash = input.find('/');
  const auto authority = input.substr(0, slash);
  out.target = slash == input.npos ? "/" : std::string(input.substr(slash));
  if (authority.empty() || authority.find('@') != authority.npos) return false;
  if (authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == authority.npos || close == 1) return false;
    out.host = authority.substr(1, close - 1);
    if (close + 1 == authority.size()) out.port = out.tls ? "443" : "80";
    else if (authority[close + 1] == ':') out.port = authority.substr(close + 2);
    else return false;
  } else {
    const size_t colon = authority.rfind(':');
    if (colon != authority.npos && authority.find(':') != colon) return false;
    out.host = authority.substr(0, colon);
    out.port = colon == authority.npos ? (out.tls ? "443" : "80") : std::string(authority.substr(colon + 1));
  }
  unsigned port = 0;
  const auto parsed = std::from_chars(out.port.data(), out.port.data() + out.port.size(), port);
  if (out.host.empty() || parsed.ec != std::errc{} || parsed.ptr != out.port.data() + out.port.size() || !port || port > 65535)
    return false;
  for (unsigned char c : out.host + out.target) if (c <= 0x20 || c == 0x7f) return false;
  return out.target.find('#') == std::string::npos;
}
bool loopback(std::string_view host) { return host == "localhost" || host == "127.0.0.1" || host == "::1"; }

enum class Wait { Ready, Cancelled, Timeout, Error };
Wait wait_socket(int fd, short events, Clock::time_point deadline, const CloudCancellation *token) {
  for (;;) {
    if (cancelled(token)) return Wait::Cancelled;
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) return Wait::Timeout;
    pollfd pfd{fd, events, 0};
    const int rc = ::poll(&pfd, 1, static_cast<int>(std::min<int64_t>(left, 20)));
    if (rc < 0 && errno == EINTR) continue;
    if (rc < 0 || (pfd.revents & POLLNVAL)) return Wait::Error;
    if (rc > 0 && (pfd.revents & (events | POLLERR | POLLHUP))) return Wait::Ready;
  }
}
CloudTransportResult wait_failure(Wait wait, uint64_t id) {
  if (wait == Wait::Cancelled) return failure(CloudTransportStatus::Cancelled, id, "request cancelled");
  if (wait == Wait::Timeout) return failure(CloudTransportStatus::TimedOut, id, "request deadline expired");
  return failure(CloudTransportStatus::TransportError, id, "socket I/O failed");
}

// DNS is performed by one bounded service thread. A caller can leave on its
// deadline/cancellation while libc finishes resolution; a stuck resolver does
// not cause an unbounded number of threads. The service joins on unload.
class Resolver {
 public:
  struct Job {
    std::string host, port;
    std::mutex mu;
    std::condition_variable cv;
    bool done{};
    addrinfo *addresses{};
    int error{};
    ~Job() { if (addresses) ::freeaddrinfo(addresses); }
  };
  Resolver() : worker_([this] { run(); }) {}
  ~Resolver() { { std::lock_guard lock(mu_); stop_ = true; } cv_.notify_one(); worker_.join(); }
  std::shared_ptr<Job> submit(const Url &url) {
    std::lock_guard lock(mu_);
    if (busy_) return {};
    auto job = std::make_shared<Job>(); job->host = url.host; job->port = url.port;
    pending_ = job; busy_ = true; cv_.notify_one(); return job;
  }
 private:
  void run() {
    for (;;) {
      std::shared_ptr<Job> job;
      { std::unique_lock lock(mu_); cv_.wait(lock, [this] { return stop_ || pending_; });
        if (stop_ && !pending_) return; job = std::move(pending_); }
      addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_flags = AI_NUMERICSERV;
      addrinfo *result = nullptr;
      const int error = ::getaddrinfo(job->host.c_str(), job->port.c_str(), &hints, &result);
      { std::lock_guard lock(job->mu); job->addresses = result; job->error = error; job->done = true; }
      { std::lock_guard lock(mu_); busy_ = false; }
      job->cv.notify_all();
    }
  }
  std::mutex mu_; std::condition_variable cv_; bool busy_{}, stop_{};
  std::shared_ptr<Job> pending_; std::thread worker_;
};

struct Connection {
  int fd{-1}; SSL_CTX *ctx{}; SSL *ssl{};
  ~Connection() { if (ssl) SSL_free(ssl); if (ctx) SSL_CTX_free(ctx); if (fd >= 0) ::close(fd); }
};

bool load_key(const CloudTransportOptions &options, std::string &key) {
  const int fd = ::open(options.api_key_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st{};
  const bool safe = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid() &&
                    (st.st_mode & 0077) == 0 && st.st_size > 0 && st.st_size <= 4097;
  if (!safe) { ::close(fd); return false; }
  std::array<char, 4098> data{};
  ssize_t bytes;
  do { bytes = ::read(fd, data.data(), data.size()); } while (bytes < 0 && errno == EINTR);
  ::close(fd);
  if (bytes <= 0 || bytes > 4097) return false;
  key.assign(data.data(), static_cast<size_t>(bytes));
  OPENSSL_cleanse(data.data(), data.size());
  if (!key.empty() && key.back() == '\n') key.pop_back();
  if (!key.empty() && key.back() == '\r') key.pop_back();
  if (key.empty() || key.size() > 4096) return false;
  return std::all_of(key.begin(), key.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; });
}

std::string json_escape(std::string_view value) {
  std::string out;
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
    else if (c < 0x20) { constexpr char hex[] = "0123456789abcdef"; out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
    else out.push_back(static_cast<char>(c));
  }
  return out;
}
std::string payload_json(const CloudPayload &p) {
  return "{\"request_id\":" + std::to_string(p.request_id) + ",\"pinyin\":\"" + json_escape(p.pinyin) +
         "\",\"committed\":\"" + json_escape(p.committed) + "\",\"context_token\":\"" + json_escape(p.context_token) +
         "\",\"interval_ms\":" + std::to_string(p.interval_ms) + ",\"delete_interval_ms\":" + std::to_string(p.delete_interval_ms) + "}";
}

CloudTransportResult transport(const CloudPayload &payload, const CloudTransportOptions &options, CloudCancellation *token) {
  SigpipeGuard sigpipe;
  const auto id = payload.request_id;
  if (cancelled(token)) return wait_failure(Wait::Cancelled, id);
  if (options.timeout.count() <= 0) return wait_failure(Wait::Timeout, id);
  const auto deadline = Clock::now() + std::min(options.timeout, std::chrono::milliseconds(60000));
  Url url;
  if (!parse_url(options.endpoint, url)) return failure(CloudTransportStatus::TransportError, id, "invalid HTTP(S) endpoint");
  if (!url.tls && (!loopback(url.host) || options.include_api_key))
    return failure(CloudTransportStatus::TransportError, id, "plain HTTP is limited to loopback without credentials");
  if (options.max_response_bytes == 0 || options.max_response_bytes > 1024 * 1024)
    return failure(CloudTransportStatus::ProtocolError, id, "response limit must be within 1..1048576 bytes");
  std::string key;
  if (options.include_api_key && !load_key(options, key)) {
    if (!key.empty()) OPENSSL_cleanse(key.data(), key.size());
    return failure(CloudTransportStatus::TransportError, id, "API key file must be an owned private regular file containing a bounded token");
  }
  const std::string body = payload_json(payload);
  std::string host = url.host.find(':') == url.host.npos ? url.host : "[" + url.host + "]";
  std::string request = "POST " + url.target + " HTTP/1.1\r\nHost: " + host + ":" + url.port +
                        "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size());
  if (!key.empty()) { request += "\r\nAuthorization: Bearer " + key; OPENSSL_cleanse(key.data(), key.size()); key.clear(); }
  request += "\r\n\r\n" + body;
  struct RequestWiper { std::string &value; ~RequestWiper() { OPENSSL_cleanse(value.data(), value.size()); } } wiper{request};

  static Resolver resolver;
  auto job = resolver.submit(url);
  if (!job) return failure(CloudTransportStatus::TransportError, id, "resolver is busy; retry later");
  { std::unique_lock lock(job->mu);
    while (!job->done) {
      if (cancelled(token)) return wait_failure(Wait::Cancelled, id);
      if (Clock::now() >= deadline) return wait_failure(Wait::Timeout, id);
      job->cv.wait_until(lock, std::min(deadline, Clock::now() + std::chrono::milliseconds(20)));
    }
  }
  if (job->error) return failure(CloudTransportStatus::TransportError, id, "name resolution failed");
  Connection connection;
  for (auto *entry = job->addresses; entry; entry = entry->ai_next) {
    if (cancelled(token)) return wait_failure(Wait::Cancelled, id);
    if (Clock::now() >= deadline) return wait_failure(Wait::Timeout, id);
    const int fd = ::socket(entry->ai_family, entry->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, entry->ai_protocol);
    if (fd < 0) continue;
    connection.fd = fd;
    if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) break;
    if (errno == EINPROGRESS) {
      const auto wait = wait_socket(fd, POLLOUT, deadline, token);
      if (wait != Wait::Ready) return wait_failure(wait, id);
      int error = 0; socklen_t size = sizeof(error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0) break;
    }
    ::close(fd); connection.fd = -1;
  }
  if (connection.fd < 0) return failure(CloudTransportStatus::TransportError, id, "connection failed");

  if (url.tls) {
    connection.ctx = SSL_CTX_new(TLS_client_method());
    if (!connection.ctx || SSL_CTX_set_min_proto_version(connection.ctx, TLS1_2_VERSION) != 1)
      return failure(CloudTransportStatus::TransportError, id, "TLS initialization failed");
    SSL_CTX_set_verify(connection.ctx, SSL_VERIFY_PEER, nullptr);
    const int trust = options.ca_certificate_path.empty() ? SSL_CTX_set_default_verify_paths(connection.ctx)
        : SSL_CTX_load_verify_locations(connection.ctx, options.ca_certificate_path.c_str(), nullptr);
    if (trust != 1) return failure(CloudTransportStatus::TransportError, id, "TLS trust store unavailable");
    connection.ssl = SSL_new(connection.ctx);
    if (!connection.ssl || SSL_set_fd(connection.ssl, connection.fd) != 1)
      return failure(CloudTransportStatus::TransportError, id, "TLS initialization failed");
    std::array<unsigned char, 16> numeric{};
    const bool ip = ::inet_pton(AF_INET, url.host.c_str(), numeric.data()) == 1 ||
                    ::inet_pton(AF_INET6, url.host.c_str(), numeric.data()) == 1;
    X509_VERIFY_PARAM *param = SSL_get0_param(connection.ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    const int identity = ip ? X509_VERIFY_PARAM_set1_ip_asc(param, url.host.c_str())
                            : SSL_set1_host(connection.ssl, url.host.c_str());
    if (identity != 1 || (!ip && SSL_set_tlsext_host_name(connection.ssl, url.host.c_str()) != 1))
      return failure(CloudTransportStatus::TransportError, id, "TLS identity setup failed");
    for (;;) {
      if (cancelled(token)) return wait_failure(Wait::Cancelled, id);
      if (Clock::now() >= deadline) return wait_failure(Wait::Timeout, id);
      const int rc = SSL_connect(connection.ssl);
      if (rc == 1) break;
      const int error = SSL_get_error(connection.ssl, rc);
      if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
        return failure(CloudTransportStatus::TransportError, id, "TLS handshake or certificate verification failed");
      const auto wait = wait_socket(connection.fd, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline, token);
      if (wait != Wait::Ready) return wait_failure(wait, id);
    }
    if (SSL_get_verify_result(connection.ssl) != X509_V_OK)
      return failure(CloudTransportStatus::TransportError, id, "TLS certificate verification failed");
  }

  // One absolute deadline is shared by DNS, connect, TLS, every write and
  // every read; receiving a byte never resets the timeout.
  auto io = [&](bool read, char *data, size_t bytes, ssize_t &count) -> Wait {
    for (;;) {
      if (cancelled(token)) return Wait::Cancelled;
      if (Clock::now() >= deadline) return Wait::Timeout;
      short wanted = read ? POLLIN : POLLOUT;
      if (connection.ssl) {
        const int rc = read ? SSL_read(connection.ssl, data, static_cast<int>(bytes))
                            : SSL_write(connection.ssl, data, static_cast<int>(bytes));
        if (rc > 0) { count = rc; return Wait::Ready; }
        const int error = SSL_get_error(connection.ssl, rc);
        if (error == SSL_ERROR_ZERO_RETURN) { count = 0; return Wait::Ready; }
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) return Wait::Error;
        wanted = error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
      } else {
        count = read ? ::recv(connection.fd, data, bytes, 0) : ::send(connection.fd, data, bytes, MSG_NOSIGNAL);
        if (count >= 0) return Wait::Ready;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return Wait::Error;
      }
      const auto wait = wait_socket(connection.fd, wanted, deadline, token);
      if (wait != Wait::Ready) return wait;
    }
  };
  for (size_t sent = 0; sent < request.size();) {
    ssize_t bytes = 0; const auto wait = io(false, request.data() + sent, request.size() - sent, bytes);
    if (wait != Wait::Ready) return wait_failure(wait, id);
    if (bytes <= 0) return failure(CloudTransportStatus::TransportError, id, "connection closed while sending");
    sent += static_cast<size_t>(bytes);
  }

  std::string received;
  size_t header_end = std::string::npos;
  std::optional<size_t> length;
  int status = 0;
  for (;;) {
    std::array<char, 4096> data{}; ssize_t bytes = 0;
    const auto wait = io(true, data.data(), data.size(), bytes);
    if (wait != Wait::Ready) return wait_failure(wait, id);
    if (!bytes) break;
    received.append(data.data(), static_cast<size_t>(bytes));
    if (received.size() > options.max_response_bytes + 8192)
      return failure(CloudTransportStatus::ProtocolError, id, "response exceeds limit");
    if (header_end == std::string::npos) {
      header_end = received.find("\r\n\r\n");
      if (header_end == std::string::npos) {
        if (received.size() > 8192) return failure(CloudTransportStatus::ProtocolError, id, "HTTP headers exceed limit");
        continue;
      }
      if (header_end > 8192 || received.size() < 12 || (!received.starts_with("HTTP/1.1 ") && !received.starts_with("HTTP/1.0 ")) ||
          std::from_chars(received.data() + 9, received.data() + 12, status).ec != std::errc{})
        return failure(CloudTransportStatus::ProtocolError, id, "invalid HTTP status");
      size_t begin = received.find("\r\n") + 2;
      while (begin < header_end) {
        const size_t end = received.find("\r\n", begin), colon = received.find(':', begin);
        if (colon == received.npos || colon >= end) return failure(CloudTransportStatus::ProtocolError, id, "invalid HTTP header");
        std::string name = received.substr(begin, colon - begin);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; });
        size_t value = colon + 1, value_end = end;
        while (value < value_end && (received[value] == ' ' || received[value] == '\t')) ++value;
        while (value_end > value && (received[value_end - 1] == ' ' || received[value_end - 1] == '\t')) --value_end;
        if (name == "transfer-encoding") return failure(CloudTransportStatus::ProtocolError, id, "transfer encoding unsupported");
        if (name == "content-length") {
          size_t parsed = 0; const auto result = std::from_chars(received.data() + value, received.data() + value_end, parsed);
          if (length || result.ec != std::errc{} || result.ptr != received.data() + value_end || parsed > options.max_response_bytes)
            return failure(CloudTransportStatus::ProtocolError, id, "invalid Content-Length");
          length = parsed;
        }
        begin = end + 2;
      }
    }
    const size_t body_size = received.size() - header_end - 4;
    if (body_size > options.max_response_bytes) return failure(CloudTransportStatus::ProtocolError, id, "response body exceeds limit");
    if (length && body_size >= *length) break;
  }
  if (header_end == std::string::npos || (length && received.size() - header_end - 4 < *length))
    return failure(CloudTransportStatus::ProtocolError, id, "incomplete HTTP response");
  CloudTransportResult result{status >= 200 && status < 300 ? CloudTransportStatus::Sent : CloudTransportStatus::TransportError,
                              status, id, received.substr(header_end + 4, length.value_or(std::string::npos)), {}};
  if (!result.ok()) result.detail = "HTTP status " + std::to_string(status);
  return result;
}
} // namespace

CloudTransportResult send_cloud_request(const Event &event, const CloudTransportOptions &options, bool consent,
                                        CloudCancellation *cancellation, uint64_t request_id) {
  if (!options.enabled || options.endpoint.empty()) return failure(CloudTransportStatus::Disabled, request_id, "cloud transport disabled");
  if (!consent) return failure(CloudTransportStatus::ConsentRequired, request_id, "explicit consent required");
  const auto payload = make_cloud_payload(event, true, request_id);
  if (!payload) return failure(CloudTransportStatus::InvalidPayload, request_id, "event is not an eligible semantic payload");
  return transport(*payload, options, cancellation);
}

CloudTransportResult send_cloud_payload(const CloudPayload &payload, const CloudTransportOptions &options, bool consent,
                                        CloudCancellation *cancellation) {
  if (!options.enabled || options.endpoint.empty()) return failure(CloudTransportStatus::Disabled, payload.request_id, "cloud transport disabled");
  if (!consent) return failure(CloudTransportStatus::ConsentRequired, payload.request_id, "explicit consent required");
  if (!cloud_detail::valid_pinyin(payload.pinyin) ||
      !cloud_detail::valid_text(payload.committed, 512) ||
      !cloud_detail::valid_context_token(payload.context_token))
    return failure(CloudTransportStatus::InvalidPayload, payload.request_id, "invalid cloud payload");
  return transport(payload, options, cancellation);
}
} // namespace tipe
