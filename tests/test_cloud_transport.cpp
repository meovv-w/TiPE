#include "tipe/learning.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

namespace {

struct LoopbackServer {
  int fd{-1};
  uint16_t port{};
  std::thread worker;
  std::string request;
  int sleep_ms{};

  explicit LoopbackServer(int delay = 0) : sleep_ms(delay) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int reuse = 1;
    assert(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0);
    port = ntohs(addr.sin_port);
    assert(::listen(fd, 1) == 0);
    worker = std::thread([this] {
      const int client = ::accept(fd, nullptr, nullptr);
      if (client < 0) return;
      char data[2048];
      while (true) {
        const ssize_t n = ::recv(client, data, sizeof(data), 0);
        if (n <= 0) break;
        request.append(data, static_cast<size_t>(n));
        if (request.find("\r\n\r\n") != std::string::npos) break;
      }
      if (sleep_ms) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      const std::string body = "{\"ok\":true}";
      const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
      ::close(client);
    });
  }

  ~LoopbackServer() {
    // Wake accept when a test intentionally made no request (for example the
    // pre-cancelled case), then let the worker close its side cleanly.
    if (fd >= 0) {
      const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
      if (wake >= 0) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        (void)::connect(wake, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        ::close(wake);
      }
      ::close(fd);
      fd = -1;
    }
    if (worker.joinable()) worker.join();
  }
};

struct TlsMaterial {
  std::string dir, cert, key;
  TlsMaterial() {
    dir = "/tmp/tipe-cloud-tls-" + std::to_string(static_cast<long long>(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    cert = dir + "/cert.pem"; key = dir + "/key.pem";
    EVP_PKEY_CTX *key_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    assert(key_ctx && EVP_PKEY_keygen_init(key_ctx) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(key_ctx, 2048) == 1);
    EVP_PKEY *pkey = nullptr; assert(EVP_PKEY_keygen(key_ctx, &pkey) == 1); EVP_PKEY_CTX_free(key_ctx);
    X509 *x509 = X509_new(); assert(x509);
    assert(X509_set_version(x509, 2) == 1); ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), -60); X509_gmtime_adj(X509_get_notAfter(x509), 3600);
    assert(X509_set_pubkey(x509, pkey) == 1);
    X509_NAME *name = X509_get_subject_name(x509);
    assert(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0) == 1);
    assert(X509_set_issuer_name(x509, name) == 1);
    X509V3_CTX ext_ctx; X509V3_set_ctx_nodb(&ext_ctx); X509V3_set_ctx(&ext_ctx, x509, x509, nullptr, nullptr, 0);
    X509_EXTENSION *san = X509V3_EXT_conf_nid(nullptr, &ext_ctx, NID_subject_alt_name,
                                               const_cast<char *>("DNS:localhost"));
    assert(san && X509_add_ext(x509, san, -1) == 1); X509_EXTENSION_free(san);
    assert(X509_sign(x509, pkey, EVP_sha256()) > 0);
    FILE *cert_file = std::fopen(cert.c_str(), "wb"), *key_file = std::fopen(key.c_str(), "wb");
    assert(cert_file && key_file); assert(PEM_write_X509(cert_file, x509) == 1);
    assert(PEM_write_PrivateKey(key_file, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1);
    std::fclose(cert_file); std::fclose(key_file); ::chmod(cert.c_str(), 0600); ::chmod(key.c_str(), 0600);
    X509_free(x509); EVP_PKEY_free(pkey);
  }
  ~TlsMaterial() { std::filesystem::remove_all(dir); }
};

struct TlsServer {
  int fd{-1}; uint16_t port{}; std::thread worker; std::string request;
  explicit TlsServer(const TlsMaterial &material) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0); int reuse = 1;
    assert(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = 0;
    assert(::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0); socklen_t len = sizeof(addr);
    assert(::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0); port = ntohs(addr.sin_port); assert(::listen(fd, 1) == 0);
    worker = std::thread([this, cert = material.cert, key = material.key] {
      const int client = ::accept(fd, nullptr, nullptr); if (client < 0) return;
      SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
      assert(ctx && SSL_CTX_use_certificate_file(ctx, cert.c_str(), SSL_FILETYPE_PEM) == 1 &&
             SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM) == 1);
      SSL *ssl = SSL_new(ctx); assert(ssl && SSL_set_fd(ssl, client) == 1);
      if (SSL_accept(ssl) == 1) {
        char data[4096];
        while (true) { const int n = SSL_read(ssl, data, sizeof(data)); if (n <= 0) break; request.append(data, static_cast<size_t>(n));
          if (request.find("\r\n\r\n") != std::string::npos) break; }
        const std::string body = "{\"tls\":true}";
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        (void)SSL_write(ssl, response.data(), static_cast<int>(response.size()));
      }
      SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); ::close(client);
    });
  }
  ~TlsServer() {
    if (fd >= 0) { const int wake = ::socket(AF_INET, SOCK_STREAM, 0); if (wake >= 0) {
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(port);
        (void)::connect(wake, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)); ::close(wake); }
      ::close(fd); fd = -1; }
    if (worker.joinable()) worker.join();
  }
};

} // namespace

int main() {
  tipe::Event event;
  event.type = tipe::EventType::Commit;
  event.pinyin = "nihao";
  event.committed = "你好";
  event.context = "/private/editor";
  event.key = "RAW_KEY_MUST_NOT_LEAK";
  event.preedit = "nihao";
  event.interval_ms = 8;

  tipe::CloudTransportOptions disabled;
  auto result = tipe::send_cloud_request(event, disabled, true, nullptr, 11);
  assert(result.status == tipe::CloudTransportStatus::Disabled);

  LoopbackServer server;
  tipe::CloudTransportOptions options;
  options.enabled = true;
  options.endpoint = "http://127.0.0.1:" + std::to_string(server.port) + "/v1/enhance";
  options.timeout = std::chrono::milliseconds(1000);
  result = tipe::send_cloud_request(event, options, false, nullptr, 12);
  assert(result.status == tipe::CloudTransportStatus::ConsentRequired);
  result = tipe::send_cloud_request(event, options, true, nullptr, 13);
  assert(result.ok() && result.http_status == 200 && result.response == "{\"ok\":true}");
  assert(server.request.find("RAW_KEY_MUST_NOT_LEAK") == std::string::npos);
  assert(server.request.find("/private/editor") == std::string::npos);
  assert(server.request.find("\"pinyin\":\"nihao\"") != std::string::npos);
  assert(server.request.find("\"committed\":\"你好\"") != std::string::npos);
  assert(server.request.find("context_token") != std::string::npos);

  // A local certificate trusted through an explicit CA file succeeds and
  // carries the opt-in API key. Hostname validation rejects the same cert
  // when addressed as 127.0.0.1 (it only contains DNS:localhost).
  {
    TlsMaterial material;
    TlsServer tls_server(material);
    options.endpoint = "https://localhost:" + std::to_string(tls_server.port) + "/v1/enhance";
    options.ca_certificate_path = material.cert;
    options.include_api_key = true;
    const std::string key_path = material.dir + "/api-key";
    { std::ofstream key(key_path); key << "test-secret\n"; }
    ::chmod(key_path.c_str(), 0600);
    options.api_key_path = key_path;
    result = tipe::send_cloud_request(event, options, true, nullptr, 130);
    assert(result.ok() && result.response == "{\"tls\":true}");
    assert(tls_server.request.find("Authorization: Bearer test-secret") != std::string::npos);
    assert(tls_server.request.find("RAW_KEY_MUST_NOT_LEAK") == std::string::npos);
    TlsServer untrusted_server(material);
    options.endpoint = "https://localhost:" + std::to_string(untrusted_server.port) + "/v1";
    options.ca_certificate_path.clear();
    options.include_api_key = false; options.api_key_path.clear();
    result = tipe::send_cloud_request(event, options, true, nullptr, 1300);
    assert(result.status == tipe::CloudTransportStatus::TransportError);
    TlsServer mismatch_server(material);
    options.endpoint = "https://127.0.0.1:" + std::to_string(mismatch_server.port) + "/v1";
    options.ca_certificate_path = material.cert;
    result = tipe::send_cloud_request(event, options, true, nullptr, 1301);
    assert(result.status == tipe::CloudTransportStatus::TransportError);
    options.include_api_key = false; options.api_key_path.clear(); options.ca_certificate_path.clear();
  }

  // Remote clear-text HTTP remains rejected, while HTTPS is accepted by the
  // OpenSSL path (this endpoint intentionally has no listener, so connection
  // failure is expected after URL/TLS setup validation).
  options.endpoint = "http://example.com:80/v1";
  result = tipe::send_cloud_request(event, options, true, nullptr, 131);
  assert(result.status == tipe::CloudTransportStatus::TransportError);
  options.endpoint = "https://127.0.0.1:443/v1";
  result = tipe::send_cloud_request(event, options, true, nullptr, 132);
  assert(result.status == tipe::CloudTransportStatus::TransportError);

  // API keys are only read from an owner-only regular file. The permission
  // check runs before any network connection is attempted.
  const std::string key_path = "/tmp/tipe-cloud-key-" + std::to_string(::getpid());
  { std::ofstream key(key_path); key << "test-secret\n"; }
  (void)::chmod(key_path.c_str(), 0644);
  options.endpoint = "https://127.0.0.1:1/v1";
  options.include_api_key = true;
  options.api_key_path = key_path;
  result = tipe::send_cloud_request(event, options, true, nullptr, 134);
  assert(result.status == tipe::CloudTransportStatus::TransportError);
  assert(result.detail.find("API key file") != std::string::npos);
  (void)::chmod(key_path.c_str(), 0600);
  result = tipe::send_cloud_request(event, options, true, nullptr, 135);
  assert(result.status == tipe::CloudTransportStatus::TransportError);
  assert(result.detail.find("API key file") == std::string::npos);
  (void)::unlink(key_path.c_str());
  options.include_api_key = false;
  options.api_key_path.clear();

  // The response body is bounded before it is returned to a caller.
  LoopbackServer large_response_server;
  options.endpoint = "http://127.0.0.1:" + std::to_string(large_response_server.port) + "/";
  options.max_response_bytes = 4;
  result = tipe::send_cloud_request(event, options, true, nullptr, 133);
  assert(result.status == tipe::CloudTransportStatus::ProtocolError);
  options.max_response_bytes = 64 * 1024;

  // A pre-cancelled request must never open a connection or block.
  tipe::CloudCancellation cancelled;
  cancelled.cancel();
  LoopbackServer cancellation_server;
  options.endpoint = "http://127.0.0.1:" + std::to_string(cancellation_server.port) + "/";
  result = tipe::send_cloud_request(event, options, true, &cancelled, 14);
  assert(result.status == tipe::CloudTransportStatus::Cancelled);
  assert(cancellation_server.request.empty());

  // Read cancellation/timeout remains bounded even when the peer stalls.
  LoopbackServer slow_server(250);
  options.endpoint = "http://127.0.0.1:" + std::to_string(slow_server.port) + "/";
  options.timeout = std::chrono::milliseconds(40);
  const auto start = std::chrono::steady_clock::now();
  result = tipe::send_cloud_request(event, options, true, nullptr, 15);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  assert(result.status == tipe::CloudTransportStatus::TimedOut);
  assert(elapsed.count() < 220);

  LoopbackServer cancellable_server(500);
  options.endpoint = "http://127.0.0.1:" + std::to_string(cancellable_server.port) + "/";
  options.timeout = std::chrono::milliseconds(1000);
  tipe::CloudCancellation in_flight_cancel;
  tipe::CloudTransportResult cancelled_result;
  std::thread request_thread([&] {
    cancelled_result = tipe::send_cloud_request(event, options, true, &in_flight_cancel, 16);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  in_flight_cancel.cancel();
  request_thread.join();
  assert(cancelled_result.status == tipe::CloudTransportStatus::Cancelled);
  return 0;
}
