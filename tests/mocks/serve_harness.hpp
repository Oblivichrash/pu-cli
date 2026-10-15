// SPDX-License-Identifier: GPL-3.0-only
#pragma once

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
#include <iterator>
#include <memory>
#include <optional>
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

#include "pu/app/cli.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime/runtime.hpp"
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

class FakeHttpServer {
 public:
  using Responder =
      std::function<http::response<http::string_body>(const http::request<http::string_body>&)>;

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

class FakeBackend : public FakeHttpServer {
 public:
  explicit FakeBackend(int response_delay_ms = 0) : FakeHttpServer(Answer, response_delay_ms) {}

  // The answer is built by a static function, so the counts a test wants to observe
  // live where that function can read them.
  static void ReportUsage(int prompt_tokens, int completion_tokens) {
    prompt_tokens_ = prompt_tokens;
    completion_tokens_ = completion_tokens;
  }

 private:
  static http::response<http::string_body> Answer(const http::request<http::string_body>& req) {
    http::response<http::string_body> res;
    if (req.target() == "/api/chat" && req.method() == http::verb::post) {
      res.result(http::status::ok);
      res.set(http::field::content_type, "application/json");
      res.body() = R"({"message":{"content":"OK"}}
{"done":true,"prompt_eval_count":)" + std::to_string(prompt_tokens_) +
                   R"(,"eval_count":)" + std::to_string(completion_tokens_) + R"(}
)";
    } else {
      res.result(http::status::not_found);
    }
    res.prepare_payload();
    return res;
  }

  static inline int prompt_tokens_ = 1234;
  static inline int completion_tokens_ = 56;
};

// The name is a string, and this says so instead of leaving it to a conversion. A brace-init
// list of char* here was read as something other than a string on CI's Boost, so the config
// serialized without the name and startup threw "Missing agent name"; assigning a constructed
// value is what made those tests pass.
inline boost::json::value JsonStr(const std::string& s) {
  return boost::json::value(boost::json::string(s));
}

inline std::string WriteAgentsFile(const fs::path& dir, int backend_port,
                                   const std::string& backend_type = "ollama",
                                   const std::string& mcp_name = "",
                                   const std::string& mcp_url = "") {
  std::error_code ec;
  fs::create_directories(dir / ".pu", ec);
  REQUIRE_FALSE(ec);
  fs::path path = dir / ".pu" / "agents.json";

  boost::json::object backend;
  backend["gateway"] = JsonStr(backend_type);
  backend["host"] = JsonStr("http://127.0.0.1:" + std::to_string(backend_port));
  backend["model"] = JsonStr("test-model");

  boost::json::object security;
  security["sandbox_root"] = JsonStr(".");
  security["forbidden_patterns"] = boost::json::array{};

  boost::json::object agent;
  agent["name"] = JsonStr("chat");
  agent["description"] = JsonStr("Chat agent");
  agent["backend"] = std::move(backend);
  agent["security"] = std::move(security);

  if (!mcp_url.empty()) {
    boost::json::object server;
    server["name"] = JsonStr(mcp_name);
    server["url"] = JsonStr(mcp_url);
    agent["mcp_servers"] = boost::json::array{std::move(server)};
  }

  boost::json::object root;
  root["default_agent"] = JsonStr("chat");
  root["agents"] = boost::json::array{std::move(agent)};

  const std::string text = boost::json::serialize(root);
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << text;
    REQUIRE(file.good());
  }
  return path.string();
}

struct HttpResponse {
  unsigned status = 0;
  std::string body;
};

class TestHttpClient {
 public:
  explicit TestHttpClient(const std::string& host, int port) : host_(host), port_(port) {}

  std::string Get(const std::string& path) { return Request(http::verb::get, path, "").body; }

  HttpResponse GetRaw(const std::string& path) { return Request(http::verb::get, path, ""); }

  std::string Post(const std::string& path, const boost::json::value& body) {
    return PostFull(path, body).body;
  }

  HttpResponse PostFull(const std::string& path, const boost::json::value& body) {
    return Request(http::verb::post, path, boost::json::serialize(body));
  }

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
  struct McpServer {
    std::string name;
    std::string url;
  };

  explicit ServeHarness(const std::string& backend_type = "ollama", int backend_delay_ms = 0,
                        std::optional<McpServer> mcp = std::nullopt) {
    static std::atomic<int> seq{0};
    std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "_" + std::to_string(seq.fetch_add(1));
    home_ = UniqueTempPath("pu_serve_" + tag);
    fs::create_directories(home_ / ".pu");

    home_env_ = std::make_unique<ScopedEnvVar>("HOME", home_.string());
    const fs::path data_dir = UniqueTempPath("pu_test_data");
    fs::create_directories(data_dir);
    data_env_ = std::make_unique<ScopedEnvVar>("PU_HOME", data_dir.string());

    backend_ = std::make_unique<FakeBackend>(backend_delay_ms);

    WriteAgentsFile(home_, backend_->Port(), backend_type, mcp ? mcp->name : "",
                    mcp ? mcp->url : "");

    {
      // Read back what was written, the way the runtime will. A config the harness cannot
      // round-trip here would surface deeper as an unrelated startup error.
      std::ifstream written(home_ / ".pu" / "agents.json", std::ios::binary);
      REQUIRE(written.is_open());
      const std::string text((std::istreambuf_iterator<char>(written)),
                             std::istreambuf_iterator<char>());
      CAPTURE(text);
      const boost::json::value parsed = boost::json::parse(text);
      const boost::json::value& first = parsed.at("agents").at(0).at("name");
      REQUIRE(first.is_string());
      REQUIRE(boost::json::value_to<std::string>(first) == "chat");
    }

    {
      ScopedWorkingDir in_home(home_);
      runtime_ = std::make_unique<pu::Runtime>();
      runtime_->Initialize();
    }

    port_ = FindFreePort();
    REQUIRE(port_ > 0);

    server_thread_ =
        std::thread([this] { cli::RunServe(kServeHost, static_cast<std::uint16_t>(port_), *runtime_); });

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

  const fs::path& Home() const { return home_; }

  TestHttpClient Client() const { return TestHttpClient(kServeHost, port_); }

  int Port() const { return port_; }

  int BackendRequests() const { return backend_->Requests(); }

  Runtime& RuntimeRef() { return *runtime_; }

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
