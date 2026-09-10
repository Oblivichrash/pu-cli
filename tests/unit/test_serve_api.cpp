// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/cli.hpp"
#include "pu/runtime.hpp"
#include "pu/core/platform.hpp"
#include "pu/infra/beast_http_client.hpp"
#include "tests/mocks/test_helpers.hpp"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
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

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace pu::platform {
extern std::atomic<bool> interrupted;
}

// Boost.Beast aliases used throughout this test file.
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

static constexpr const char* kHost = "127.0.0.1";

namespace {

int FindFreePort() {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) { WSACleanup(); return 0; }
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

bool WaitForPort(const std::string& host, int port, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { WSACleanup(); return false; }
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

class FakeBackend {
public:
  FakeBackend() {
    ioc_ = std::make_shared<net::io_context>();
    acceptor_ = std::make_shared<tcp::acceptor>(
        *ioc_,
        tcp::endpoint(net::ip::make_address("127.0.0.1"), 0)
    );
    port_ = acceptor_->local_endpoint().port();
    REQUIRE(port_ > 0);

    DoAccept();
    server_thread_ = std::thread([this] { ioc_->run(); });
    REQUIRE(WaitForPort("127.0.0.1", port_, 10000));
  }

  ~FakeBackend() { Stop(); }

  int Port() const { return port_; }

  void Stop() {
    if (server_thread_.joinable()) {
      ioc_->stop();
      server_thread_.join();
    }
  }

private:
  void DoAccept() {
    acceptor_->async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
          if (!ec) {
            std::thread([s = std::move(socket)]() mutable { HandleRequest(std::move(s)); }).detach();
          }
          if (!stop_requested_) DoAccept();
        }
    );
  }

  static void HandleRequest(tcp::socket socket) {
    try {
      beast::flat_buffer buffer;
      http::request<http::string_body> req;
      http::read(socket, buffer, req);

      http::response<http::string_body> res;
      if (req.target() == "/api/chat" && req.method() == http::verb::post) {
        res.result(http::status::ok);
        res.set(http::field::content_type, "application/json");
        res.body() = R"({"message":{"content":"OK"}}
{"done":true}
)";
        res.prepare_payload();
      } else {
        res.result(http::status::not_found);
        res.prepare_payload();
      }

      http::write(socket, res);
      socket.shutdown(tcp::socket::shutdown_send);
    } catch (const std::exception& e) {
      spdlog::warn("FakeBackend request error: {}", e.what());
    }
  }

  std::shared_ptr<net::io_context> ioc_;
  std::shared_ptr<tcp::acceptor> acceptor_;
  int port_ = 0;
  std::thread server_thread_;
  std::atomic<bool> stop_requested_{false};
};

std::string WriteAgentsFile(const fs::path& dir, int backend_port) {
  fs::create_directories(dir / ".pu");
  fs::path path = dir / ".pu" / "agents.json";

  boost::json::value root = {
      {"default_agent", "chat"},
      {"agents", boost::json::array{
          boost::json::value{
              {"name", "chat"},
              {"description", "Chat agent"},
              {"backend", {
                  {"type", "ollama"},
                  {"host", "http://127.0.0.1:" + std::to_string(backend_port)},
                  {"model", "test-model"}
              }},
              {"security", {
                  {"sandbox_root", "."},
                  {"allowed_paths", boost::json::array{}},
                  {"forbidden_patterns", boost::json::array{}}
              }}
          }
      }}
  };

  std::ofstream file(path);
  file << boost::json::serialize(root);
  return path.string();
}

class TestHttpClient {
public:
  explicit TestHttpClient(const std::string& host, int port)
      : host_(host), port_(port) {}

  std::string Get(const std::string& path) {
    return Request(http::verb::get, path, "");
  }

  std::string Post(const std::string& path, const boost::json::value& body) {
    return Request(http::verb::post, path, boost::json::serialize(body));
  }

private:
  std::string Request(http::verb method, const std::string& path,
                      const std::string& body) {
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

      return res.body();
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("HTTP request failed: ") + e.what());
    }
  }

  std::string host_;
  int port_;
};

class ServeHarness {
public:
  ServeHarness() {
    static std::atomic<int> seq{0};
    std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "_" + std::to_string(seq.fetch_add(1));
    home_ = fs::temp_directory_path() / ("pu_serve_" + tag);
    fs::create_directories(home_ / ".pu");

    home_env_ = std::make_unique<pu::tests::ScopedEnvVar>("HOME", home_.string());
    data_env_ = std::make_unique<pu::tests::ScopedEnvVar>("PU_HOME", home_.string());

    backend_ = std::make_unique<FakeBackend>();
    WriteAgentsFile(home_, backend_->Port());

    port_ = FindFreePort();
    REQUIRE(port_ > 0);

    runtime_ = std::make_unique<pu::Runtime>();
    server_thread_ = std::thread([this] {
      pu::cli::RunServe(kHost, port_, *runtime_);
    });

    REQUIRE(WaitForPort(kHost, port_, 15000));
  }

  ~ServeHarness() {
    Stop();
    std::error_code ec;
    fs::remove_all(home_, ec);
  }

  void Stop() {
    if (stopped_) return;
    stopped_ = true;
    pu::platform::interrupted = true;
    if (server_thread_.joinable()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      server_thread_.join();
    }
    pu::platform::interrupted = false;
    if (runtime_) runtime_->Shutdown();
  }

  TestHttpClient Client() const { return TestHttpClient(kHost, port_); }

  int Port() const { return port_; }

private:
  fs::path home_;
  std::unique_ptr<pu::tests::ScopedEnvVar> home_env_;
  std::unique_ptr<pu::tests::ScopedEnvVar> data_env_;
  std::unique_ptr<FakeBackend> backend_;
  int port_ = 0;
  std::unique_ptr<pu::Runtime> runtime_;
  std::thread server_thread_;
  bool stopped_ = false;
};

boost::json::value ParseJson(const std::string& s) {
  return boost::json::parse(s);
}

}  // namespace

TEST_CASE("serve API /api/session", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string body = client.Get("/api/session");
  auto j = ParseJson(body);

  REQUIRE(j.at("ok") == true);
  REQUIRE(j.at("agent_name") == "chat");
  REQUIRE(j.at("backend_type") == "ollama");
  REQUIRE(j.at("backend_model") == "test-model");
}

TEST_CASE("serve API /api/history initially empty", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string body = client.Get("/api/history");
  auto j = ParseJson(body);

  REQUIRE(j.is_array());
  REQUIRE(j.as_array().empty());
}

TEST_CASE("serve API /api/agents", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string body = client.Get("/api/agents");
  auto j = ParseJson(body);

  REQUIRE(j.at("agents").is_array());
  auto& agents = j.at("agents").as_array();
  REQUIRE(agents.size() == 1);
  REQUIRE(agents[0].at("name") == "chat");
}

TEST_CASE("serve API /api/clear", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string clear_body = client.Post("/api/clear", boost::json::object{});
  auto clear_j = ParseJson(clear_body);
  REQUIRE(clear_j.at("success") == true);

  std::string history_body = client.Get("/api/history");
  auto history_j = ParseJson(history_body);
  REQUIRE(history_j.is_array());
  REQUIRE(history_j.as_array().empty());
}

TEST_CASE("serve API invalid JSON returns 400", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  try {
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto endpoints = resolver.resolve(kHost, std::to_string(harness.Port()));
    stream.connect(endpoints);

    http::request<http::string_body> req(http::verb::post, "/api/agent/switch", 11);
    req.set(http::field::host, kHost);
    req.set(http::field::content_type, "application/json");
    req.body() = "not json";
    req.prepare_payload();

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    REQUIRE(res.result_int() == 400);
    auto j = ParseJson(res.body());
    REQUIRE(j.at("success") == false);
    REQUIRE(j.at("error") == "Invalid JSON");

    stream.socket().shutdown(tcp::socket::shutdown_both);
  } catch (const std::exception& e) {
    FAIL("HTTP request failed: " << e.what());
  }
}
