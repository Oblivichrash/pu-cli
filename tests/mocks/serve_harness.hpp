// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// What a test needs to run a server for real: a server that answers with what the test
// says, a backend that answers like a provider, a workspace with an agents.json, a REST
// client, and `pu serve` itself.

#include <catch2/catch_test_macros.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "pu/cli.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime.hpp"
#include "tests/mocks/test_helpers.hpp"

namespace pu::platform {
extern std::atomic<bool> interrupted;
}

namespace pu::tests {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace fs = std::filesystem;
using tcp = net::ip::tcp;

inline constexpr const char* kServeHost = "127.0.0.1";

// A port with nothing on it yet; the server is started on it a moment later.
inline int FindFreePort() {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) {
    WSACleanup();
    return 0;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(s);
    WSACleanup();
    return 0;
  }
  int len = sizeof(addr);
  if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
    closesocket(s);
    WSACleanup();
    return 0;
  }
  int port = ntohs(addr.sin_port);
  closesocket(s);
  WSACleanup();
  return port;
#else
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(s);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    close(s);
    return 0;
  }
  int port = ntohs(addr.sin_port);
  close(s);
  return port;
#endif
}

inline bool WaitForPort(const std::string& host, int port, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
      WSACleanup();
      return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    int result = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    closesocket(s);
    WSACleanup();
    if (result == 0) return true;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    int result = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(s);
    if (result == 0) return true;
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return false;
}

// A server that answers every request with what its responder returns. `response_delay_ms`
// holds the answer back, which is what keeps a turn in flight long enough to interrupt it,
// and `Requests()` counts what reached it.
class FakeHttpServer {
 public:
  using Responder =
      std::function<http::response<http::string_body>(const http::request<http::string_body>&)>;

  // One status and body for every request.
  FakeHttpServer(unsigned status, std::string body, int response_delay_ms = 0)
      : FakeHttpServer(
            [status, body = std::move(body)](const http::request<http::string_body>&) {
              http::response<http::string_body> res;
              res.result(static_cast<http::status>(status));
              res.set(http::field::content_type, "application/json");
              res.body() = body;
              res.prepare_payload();
              return res;
            },
            response_delay_ms) {}

  explicit FakeHttpServer(Responder responder, int response_delay_ms = 0)
      : responder_(std::make_shared<Responder>(std::move(responder))),
        response_delay_ms_(response_delay_ms),
        requests_(std::make_shared<std::atomic<int>>(0)) {
    ioc_ = std::make_shared<net::io_context>();
    acceptor_ = std::make_shared<tcp::acceptor>(
        *ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0));
    port_ = acceptor_->local_endpoint().port();
    REQUIRE(port_ > 0);

    Accept();
    server_thread_ = std::thread([this] { ioc_->run(); });
    REQUIRE(WaitForPort("127.0.0.1", port_, 10000));
  }

  ~FakeHttpServer() { Stop(); }

  FakeHttpServer(const FakeHttpServer&) = delete;
  FakeHttpServer& operator=(const FakeHttpServer&) = delete;

  int Port() const { return port_; }

  // How many requests have been read, which is a turn that got as far as the backend.
  int Requests() const { return requests_->load(); }

  void Stop() {
    if (stop_requested_.exchange(true)) return;
    if (server_thread_.joinable()) {
      ioc_->stop();
      server_thread_.join();
    }
  }

 private:
  void Accept() {
    acceptor_->async_accept([this](boost::system::error_code ec, tcp::socket socket) {
      if (!ec) {
        std::thread([s = std::move(socket), responder = responder_, delay = response_delay_ms_,
                     requests = requests_]() mutable {
          Serve(std::move(s), responder, delay, requests);
        }).detach();
      }
      if (!stop_requested_) Accept();
    });
  }

  static void Serve(tcp::socket socket, const std::shared_ptr<Responder>& responder,
                    int response_delay_ms, const std::shared_ptr<std::atomic<int>>& requests) {
    try {
      beast::flat_buffer buffer;
      http::request<http::string_body> req;
      http::read(socket, buffer, req);

      requests->fetch_add(1);
      if (response_delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(response_delay_ms));
      }

      http::response<http::string_body> res = (*responder)(req);
      http::write(socket, res);

      beast::error_code ec;
      socket.shutdown(tcp::socket::shutdown_send, ec);
    } catch (const std::exception& e) {
      spdlog::debug("FakeHttpServer: {}", e.what());
    }
  }

  std::shared_ptr<Responder> responder_;
  std::shared_ptr<net::io_context> ioc_;
  std::shared_ptr<tcp::acceptor> acceptor_;
  int port_ = 0;
  int response_delay_ms_ = 0;
  std::shared_ptr<std::atomic<int>> requests_;
  std::thread server_thread_;
  std::atomic<bool> stop_requested_{false};
};

// A backend that answers like a provider, in the shape the Ollama path reads back.
class FakeBackend : public FakeHttpServer {
 public:
  explicit FakeBackend(int response_delay_ms = 0) : FakeHttpServer(Answer, response_delay_ms) {}

 private:
  static http::response<http::string_body> Answer(const http::request<http::string_body>& req) {
    http::response<http::string_body> res;
    if (req.target() == "/api/chat" && req.method() == http::verb::post) {
      res.result(http::status::ok);
      res.set(http::field::content_type, "application/json");
      res.body() = R"({"message":{"content":"OK"}}
{"done":true}
)";
    } else {
      res.result(http::status::not_found);
    }
    res.prepare_payload();
    return res;
  }
};

inline std::string WriteAgentsFile(const fs::path& dir, int backend_port,
                                   const std::string& backend_type = "ollama") {
  fs::create_directories(dir / ".pu");
  fs::path path = dir / ".pu" / "agents.json";

  boost::json::value root = {
      {"default_agent", "chat"},
      {"agents",
       boost::json::array{boost::json::value{
           {"name", "chat"},
           {"description", "Chat agent"},
           {"backend",
            {{"type", backend_type},
             {"host", "http://127.0.0.1:" + std::to_string(backend_port)},
             {"model", "test-model"}}},
           {"security", {{"sandbox_root", "."}, {"forbidden_patterns", boost::json::array{}}}}}}}};

  std::ofstream file(path);
  file << boost::json::serialize(root);
  return path.string();
}

// What a request answered. The status is read where the refusal is one rather than a field
// in the body.
struct HttpResponse {
  unsigned status = 0;
  std::string body;
};

class TestHttpClient {
 public:
  explicit TestHttpClient(const std::string& host, int port) : host_(host), port_(port) {}

  std::string Get(const std::string& path) { return Request(http::verb::get, path, "").body; }

  std::string Post(const std::string& path, const boost::json::value& body) {
    return PostFull(path, body).body;
  }

  HttpResponse PostFull(const std::string& path, const boost::json::value& body) {
    return Request(http::verb::post, path, boost::json::serialize(body));
  }

  // A body that is not JSON at all, which is a refusal the handler judges itself.
  HttpResponse PostRaw(const std::string& path, const std::string& body) {
    return Request(http::verb::post, path, body);
  }

 private:
  HttpResponse Request(http::verb method, const std::string& path, const std::string& body) {
    try {
      net::io_context ioc;
      tcp::resolver resolver(ioc);
      beast::tcp_stream stream(ioc);

      auto endpoints = resolver.resolve(host_, std::to_string(port_));
      stream.connect(endpoints);

      http::request<http::string_body> req(method, path, 11);
      req.set(http::field::host, host_);
      req.set(http::field::content_type, "application/json");
      req.set(http::field::content_length, std::to_string(body.size()));
      req.body() = body;
      req.prepare_payload();

      http::write(stream, req);

      beast::flat_buffer buffer;
      http::response<http::string_body> res;
      http::read(stream, buffer, res);

      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);

      return HttpResponse{res.result_int(), res.body()};
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("HTTP request failed: ") + e.what());
    }
  }

  std::string host_;
  int port_;
};

class ServeHarness {
 public:
  // The backend type decides what a client may do, so a test asks for one that carries what
  // it needs. `backend_delay_ms` holds the answer back, so a turn can be interrupted.
  explicit ServeHarness(const std::string& backend_type = "ollama", int backend_delay_ms = 0) {
    static std::atomic<int> seq{0};
    std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "_" + std::to_string(seq.fetch_add(1));
    home_ = fs::temp_directory_path() / ("pu_serve_" + tag);
    fs::create_directories(home_ / ".pu");

    home_env_ = std::make_unique<ScopedEnvVar>("HOME", home_.string());
    data_env_ = std::make_unique<ScopedEnvVar>("PU_HOME", home_.string());

    backend_ = std::make_unique<FakeBackend>(backend_delay_ms);
    WriteAgentsFile(home_, backend_->Port(), backend_type);

    // Initialised here rather than by the server, so the workspace it reads is this
    // harness's own. RunServe initialises a second time, which is a no-op.
    {
      ScopedWorkingDir in_home(home_);
      runtime_ = std::make_unique<pu::Runtime>();
      runtime_->Initialize();
    }

    port_ = FindFreePort();
    REQUIRE(port_ > 0);

    server_thread_ = std::thread([this] { cli::RunServe(kServeHost, port_, *runtime_); });

    REQUIRE(WaitForPort(kServeHost, port_, 15000));
  }

  ~ServeHarness() {
    Stop();
    std::error_code ec;
    fs::remove_all(home_, ec);
  }

  void Stop() {
    if (stopped_) return;
    stopped_ = true;
    platform::interrupted = true;
    if (server_thread_.joinable()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      server_thread_.join();
    }
    platform::interrupted = false;
    if (runtime_) runtime_->Shutdown();
  }

  // The workspace this harness serves: the directory the server's discovery scans beside.
  const fs::path& Home() const { return home_; }

  TestHttpClient Client() const { return TestHttpClient(kServeHost, port_); }

  int Port() const { return port_; }

  // How many turns have reached the backend, so a test can act while one is in flight
  // instead of guessing at a delay.
  int BackendRequests() const { return backend_->Requests(); }

  // The runtime the server is reading, so a test can seed a conversation in the same
  // store a turn would have written to.
  Runtime& Runtime() { return *runtime_; }  // NOLINT: the name is the class it returns

 private:
  fs::path home_;
  std::unique_ptr<ScopedEnvVar> home_env_;
  std::unique_ptr<ScopedEnvVar> data_env_;
  std::unique_ptr<FakeBackend> backend_;
  int port_ = 0;
  std::unique_ptr<pu::Runtime> runtime_;
  std::thread server_thread_;
  bool stopped_ = false;
};

}  // namespace pu::tests
