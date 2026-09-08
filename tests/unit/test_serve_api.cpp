// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/cli.hpp"
#include "pu/runtime.hpp"
#include "pu/infra/platform.hpp"

#include <httplib.h>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <mutex>
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
namespace json = pu::json;
using namespace std::chrono_literals;

namespace pu::platform {
extern std::atomic<bool> interrupted;
}

static constexpr const char* kHost = "127.0.0.1";

namespace {

int FindFreePort() {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == INVALID_SOCKET) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(s);
    return 0;
  }
  int len = sizeof(addr);
  if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
    closesocket(s);
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

class ScopedEnvVar {
 public:
  ScopedEnvVar(const std::string& name, const std::string& value) : name_(name) {
    const char* prev = std::getenv(name.c_str());
    had_prev_ = (prev != nullptr);
    if (had_prev_) prev_ = prev;
    Set(value);
  }
  ~ScopedEnvVar() {
    if (had_prev_) Set(prev_);
    else Unset();
  }

 private:
  void Set(const std::string& value) {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value.c_str());
#else
    setenv(name_.c_str(), value.c_str(), 1);
#endif
  }
  void Unset() {
#ifdef _WIN32
    _putenv_s(name_.c_str(), "");
#else
    unsetenv(name_.c_str());
#endif
  }
  std::string name_;
  std::string prev_;
  bool had_prev_ = false;
};

class FakeBackend {
 public:
  FakeBackend() {
    port_ = FindFreePort();
    REQUIRE(port_ > 0);

    server_.Post("/api/chat", [this](const httplib::Request&, httplib::Response& res) {
      std::string body = "{\"message\":{\"content\":\"OK\"}}\n{\"done\":true}\n";
      res.status = 200;
      res.set_content(body, "application/json");
    });

    listen_thread_ = std::thread([this] { server_.listen("127.0.0.1", port_); });
    REQUIRE(WaitForPort("127.0.0.1", port_, 10000));
  }

  ~FakeBackend() { Stop(); }

  void Stop() {
    if (stopped_) return;
    stopped_ = true;
    server_.stop();
    if (listen_thread_.joinable()) listen_thread_.join();
  }

  int Port() const { return port_; }

 private:
  httplib::Server server_;
  int port_ = 0;
  std::thread listen_thread_;
  bool stopped_ = false;
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

class ServeHarness {
 public:
  ServeHarness() {
    static std::atomic<int> seq{0};
    std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "_" + std::to_string(seq.fetch_add(1));
    home_ = fs::temp_directory_path() / ("pu_serve_" + tag);
    fs::create_directories(home_ / ".pu");

    home_env_ = std::make_unique<ScopedEnvVar>("HOME", home_.string());
    data_env_ = std::make_unique<ScopedEnvVar>("PU_HOME", home_.string());

    backend_ = std::make_unique<FakeBackend>();
    int backend_port = backend_->Port();

    WriteAgentsFile(home_, backend_port);

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
    backend_.reset();
    home_env_.reset();
    data_env_.reset();
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

  httplib::Client Client() const {
    return httplib::Client(kHost, port_);
  }

  httplib::Result Get(const std::string& path) const {
    auto client = Client();
    return client.Get(path);
  }

  httplib::Result Post(const std::string& path, const boost::json::value& body) const {
    auto client = Client();
    std::string body_str = boost::json::serialize(body);
    return client.Post(path, body_str, "application/json");
  }

  int Port() const { return port_; }

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

boost::json::value ParseJson(const std::string& s) {
  return boost::json::parse(s);
}

} // namespace

TEST_CASE("serve API /api/session", "[serve][api]") {
  ServeHarness harness;
  auto res = harness.Get("/api/session");
  REQUIRE(res);
  REQUIRE(res->status == 200);

  auto body = ParseJson(res->body);
  REQUIRE(body.at("ok") == true);
  REQUIRE(body.at("agent_name") == "chat");
  REQUIRE(body.at("backend_type") == "ollama");
  REQUIRE(body.at("backend_model") == "test-model");
}

TEST_CASE("serve API /api/history initially empty", "[serve][api]") {
  ServeHarness harness;
  auto res = harness.Get("/api/history");
  REQUIRE(res);
  REQUIRE(res->status == 200);

  auto body = ParseJson(res->body);
  REQUIRE(body.is_array());
  REQUIRE(body.as_array().empty());
}

TEST_CASE("serve API /api/agents", "[serve][api]") {
  ServeHarness harness;
  auto res = harness.Get("/api/agents");
  REQUIRE(res);
  REQUIRE(res->status == 200);

  auto body = ParseJson(res->body);
  REQUIRE(json::HasKey(body, "agents"));
  REQUIRE(body.at("agents").is_array());
  REQUIRE(body.at("agents").as_array().size() == 1);
  REQUIRE(body.at("agents").as_array()[0].at("name") == "chat");
}

TEST_CASE("serve API /api/clear", "[serve][api]") {
  ServeHarness harness;
  auto clear_res = harness.Post("/api/clear", boost::json::object{});
  REQUIRE(clear_res);
  REQUIRE(clear_res->status == 200);
  auto clear_body = ParseJson(clear_res->body);
  REQUIRE(clear_body.at("success") == true);

  auto history_res = harness.Get("/api/history");
  REQUIRE(history_res);
  REQUIRE(history_res->status == 200);
  auto history = ParseJson(history_res->body);
  REQUIRE(history.is_array());
  REQUIRE(history.as_array().empty());
}

TEST_CASE("serve API invalid JSON returns 400", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();
  auto res = client.Post("/api/agent/switch", "not json", "application/json");
  REQUIRE(res);
  REQUIRE(res->status == 400);
  auto body = ParseJson(res->body);
  REQUIRE(body.at("success") == false);
  REQUIRE(body.at("error") == "Invalid JSON");
}
