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
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <sstream>
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
  using Responder = std::function<std::string(int)>;

  FakeBackend(Responder responder, bool slow = false)
      : responder_(std::move(responder)), slow_(slow) {
    port_ = FindFreePort();
    REQUIRE(port_ > 0);

    server_.Post("/api/chat", [this](const httplib::Request&, httplib::Response& res) {
      int n = ++request_count_;
      if (slow_) {
        ++accept_count_;
        std::unique_lock<std::mutex> lock(mu_);
        released_cv_.wait_for(lock, std::chrono::seconds(30), [this] { return released_.load(); });
      }
      std::string body = responder_(n);
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
    { std::lock_guard<std::mutex> lock(mu_); released_ = true; }
    released_cv_.notify_all();
    server_.stop();
    if (listen_thread_.joinable()) listen_thread_.join();
  }

  int Port() const { return port_; }
  int RequestCount() const { return request_count_.load(); }

  bool WaitForAccepted(int timeout_ms = 10000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (accept_count_.load() > 0) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return accept_count_.load() > 0;
  }

 private:
  httplib::Server server_;
  Responder responder_;
  bool slow_ = false;
  int port_ = 0;
  std::atomic<int> request_count_{0};
  std::atomic<int> accept_count_{0};
  std::thread listen_thread_;
  std::mutex mu_;
  std::condition_variable released_cv_;
  std::atomic<bool> released_{false};
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
      },
      boost::json::value{
        {"name", "coder"},
        {"description", "Coding agent"},
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
  ServeHarness(FakeBackend::Responder responder, bool slow_backend = false) {
    static std::atomic<int> seq{0};
    std::string tag = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "_" + std::to_string(seq.fetch_add(1));
    home_ = fs::temp_directory_path() / ("pu_serve_" + tag);
    fs::create_directories(home_ / ".pu");

    home_env_ = std::make_unique<ScopedEnvVar>("HOME", home_.string());
    data_env_ = std::make_unique<ScopedEnvVar>("PU_HOME", home_.string());

    backend_ = std::make_unique<FakeBackend>(std::move(responder), slow_backend);
    backend_port_ = backend_->Port();

    WriteAgentsFile(home_, backend_port_);

    port_ = FindFreePort();
    REQUIRE(port_ > 0);

    runtime_ = std::make_unique<pu::Runtime>();
    run_done_ = false;
    server_thread_ = std::thread([this] {
      run_rc_ = pu::cli::RunServe(kHost, port_, *runtime_);
      run_done_ = true;
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
  FakeBackend& Backend() { return *backend_; }

 private:
  fs::path home_;
  std::unique_ptr<ScopedEnvVar> home_env_;
  std::unique_ptr<ScopedEnvVar> data_env_;
  std::unique_ptr<FakeBackend> backend_;
  int backend_port_ = 0;
  int port_ = 0;
  std::unique_ptr<pu::Runtime> runtime_;
  std::thread server_thread_;
  std::atomic<bool> run_done_{false};
  int run_rc_ = 0;
  bool stopped_ = false;
};

std::vector<std::string> ParseSseEvents(const std::string& body) {
  std::vector<std::string> events;
  size_t pos = 0;
  while (pos < body.length()) {
    size_t end = body.find("\n\n", pos);
    if (end == std::string::npos) break;
    std::string frame = body.substr(pos, end - pos);
    pos = end + 2;

    if (frame.rfind("data: ", 0) == 0) {
      events.push_back(frame.substr(6));
    }
  }
  return events;
}

boost::json::value ParseJson(const std::string& s) {
  return boost::json::parse(s);
}

std::string DefaultResponder(int n) {
  if (n == 1) {
    return "{\"message\":{\"content\":\"Hello from fake LLM\"}}\n{\"done\":true}\n";
  }
  return "{\"message\":{\"content\":\"OK\"}}\n{\"done\":true}\n";
}

}  // namespace

TEST_CASE("serve API /api/chat non-streaming", "[serve][api]") {
  ServeHarness harness(DefaultResponder);

  boost::json::value req = {
    {"message", "hello"},
    {"request_id", "nonstream-1"}
  };
  auto res = harness.Post("/api/chat", req);
  REQUIRE(res);
  REQUIRE(res->status == 200);

  auto body = ParseJson(res->body);
  REQUIRE(body.at("success") == true);
  REQUIRE(body.at("is_command") == false);
  REQUIRE(body.at("request_id") == "nonstream-1");
  REQUIRE(body.at("content").is_string());
  REQUIRE(body.at("error") == "");
  REQUIRE(body.at("tool_call_count") == 0);
}

TEST_CASE("serve API /api/chat/stream SSE events", "[serve][api]") {
  auto responder = [](int n) {
    if (n == 1) {
      return std::string("{\"message\":{\"content\":\"Alpha\"}}\n")
           + "{\"message\":{\"content\":\"Beta\"}}\n"
           + "{\"message\":{\"content\":\"Gamma\"}}\n"
           + "{\"done\":true}\n";
    }
    return std::string("{\"message\":{\"content\":\"OK\"}}\n{\"done\":true}\n");
  };
  ServeHarness harness(responder);

  boost::json::value req = {
    {"message", "stream me"},
    {"request_id", "stream-1"}
  };
  auto res = harness.Post("/api/chat/stream", req);
  REQUIRE(res);
  REQUIRE(res->status == 200);

  auto events = ParseSseEvents(res->body);
  REQUIRE(events.size() >= 4);

  REQUIRE(events.back() == "[DONE]");
  const char* expected[] = {"Alpha", "Beta", "Gamma"};
  for (size_t i = 0; i < 3; ++i) {
    auto ev = ParseJson(events[i]);
    REQUIRE(ev.at("token") == expected[i]);
  }
}

TEST_CASE("serve API /api/chat/cancel", "[serve][api]") {
  auto responder = [](int) -> std::string {
    return "";
  };
  ServeHarness harness(responder, true);

  boost::json::value stream_req = {
    {"message", "please wait"},
    {"request_id", "cancel-1"}
  };

  std::string stream_body;
  std::thread stream_thread([&] {
    auto client = harness.Client();
    std::string body = boost::json::serialize(stream_req);
    auto res = client.Post("/api/chat/stream", body, "application/json");
    if (res) {
      stream_body = res->body;
    }
  });

  REQUIRE(harness.Backend().WaitForAccepted(10000));

  boost::json::value cancel_req = {
    {"request_id", "cancel-1"}
  };
  auto cancel_res = harness.Post("/api/chat/cancel", cancel_req);
  REQUIRE(cancel_res);
  REQUIRE(cancel_res->status == 200);

  auto cancel_body = ParseJson(cancel_res->body);
  REQUIRE(cancel_body.at("success") == true);

  stream_thread.join();

  REQUIRE(!stream_body.empty());
  REQUIRE(stream_body.find("[DONE]") != std::string::npos);
}

TEST_CASE("serve API /api/session", "[serve][api]") {
  ServeHarness harness(DefaultResponder);

  auto res = harness.Get("/api/session");
  REQUIRE(res);
  REQUIRE(res->status == 200);

  auto body = ParseJson(res->body);
  REQUIRE(body.at("ok") == true);
  REQUIRE(body.at("agent_name") == "chat");
  REQUIRE(body.at("backend_type") == "ollama");
  REQUIRE(body.at("backend_model") == "test-model");
}

TEST_CASE("serve API /api/history with tool calls", "[serve][api]") {
  auto responder = [](int n) -> std::string {
    if (n == 1) {
      return R"({"message":{"content":"","tool_calls":[{"function":{"name":"execute_bash","arguments":{"command":"echo tool-output"}}}]}})"
           "\n{\"done\":true}\n";
    }
    return "{\"message\":{\"content\":\"Tool completed\"}}\n{\"done\":true}\n";
  };

  ServeHarness harness(responder);

  boost::json::value req = {
    {"message", "use a tool"},
    {"request_id", "tool-1"}
  };
  auto chat_res = harness.Post("/api/chat", req);
  REQUIRE(chat_res);
  REQUIRE(chat_res->status == 200);

  auto history_res = harness.Get("/api/history");
  REQUIRE(history_res);
  REQUIRE(history_res->status == 200);

  auto history = ParseJson(history_res->body);
  REQUIRE(history.is_array());
  REQUIRE(history.as_array().size() >= 4);

  bool found_assistant_toolcall = false;
  bool found_tool_result = false;
  bool found_final_assistant = false;

  for (const auto& msg : history.as_array()) {
    std::string role = json::ValueOrDefault<std::string>(msg, "role", "");
    if (role == "assistant") {
      if (json::HasKey(msg, "tool_calls_json")) {
        std::string tc = boost::json::value_to<std::string>(msg.at("tool_calls_json"));
        if (!tc.empty()) {
          auto parsed = boost::json::parse(tc);
          if (parsed.is_array() && !parsed.as_array().empty()) {
            found_assistant_toolcall = true;
          }
        }
      }
      if (json::HasKey(msg, "content") && msg.at("content") == "Tool completed") {
        found_final_assistant = true;
      }
    }
    if (role == "tool") {
      if (json::HasKey(msg, "tool_name") && msg.at("tool_name") == "execute_bash") {
        found_tool_result = true;
      }
    }
  }

  REQUIRE(found_assistant_toolcall);
  REQUIRE(found_tool_result);
  REQUIRE(found_final_assistant);
}

TEST_CASE("serve API /api/agents and /api/agent/switch", "[serve][api]") {
  ServeHarness harness(DefaultResponder);

  auto agents_res = harness.Get("/api/agents");
  REQUIRE(agents_res);
  REQUIRE(agents_res->status == 200);

  auto agents = ParseJson(agents_res->body);
  REQUIRE(json::HasKey(agents, "agents"));
  REQUIRE(agents.at("agents").is_array());
  REQUIRE(agents.at("agents").as_array().size() == 2);

  boost::json::value switch_req = {
    {"agent_name", "coder"}
  };
  auto switch_res = harness.Post("/api/agent/switch", switch_req);
  REQUIRE(switch_res);
  REQUIRE(switch_res->status == 200);

  auto switch_body = ParseJson(switch_res->body);
  REQUIRE(switch_body.at("success") == true);
  REQUIRE(switch_body.at("agent") == "coder");

  auto session_res = harness.Get("/api/session");
  REQUIRE(session_res);
  REQUIRE(session_res->status == 200);
  auto session = ParseJson(session_res->body);
  REQUIRE(session.at("agent_name") == "coder");

  boost::json::value bad_switch = {
    {"agent_name", "nonexistent"}
  };
  auto bad_res = harness.Post("/api/agent/switch", bad_switch);
  REQUIRE(bad_res);
  REQUIRE(bad_res->status == 404);
}

TEST_CASE("serve API /api/clear", "[serve][api]") {
  ServeHarness harness(DefaultResponder);

  boost::json::value req = {
    {"message", "hello"},
    {"request_id", "clear-1"}
  };
  auto chat_res = harness.Post("/api/chat", req);
  REQUIRE(chat_res);
  REQUIRE(chat_res->status == 200);

  auto history_before = harness.Get("/api/history");
  REQUIRE(history_before);
  auto before = ParseJson(history_before->body);
  REQUIRE(before.is_array());
  REQUIRE(before.as_array().size() >= 1);

  auto clear_res = harness.Post("/api/clear", boost::json::object{});
  REQUIRE(clear_res);
  REQUIRE(clear_res->status == 200);
  auto clear_body = ParseJson(clear_res->body);
  REQUIRE(clear_body.at("success") == true);

  auto history_after = harness.Get("/api/history");
  REQUIRE(history_after);
  auto after = ParseJson(history_after->body);
  REQUIRE(after.is_array());
  REQUIRE(after.as_array().empty());
}

TEST_CASE("serve API invalid JSON returns 400", "[serve][api]") {
  ServeHarness harness(DefaultResponder);

  auto client = harness.Client();
  auto res = client.Post("/api/chat", "not json", "application/json");
  REQUIRE(res);
  REQUIRE(res->status == 400);
  auto body = ParseJson(res->body);
  REQUIRE(body.at("success") == false);
  REQUIRE(body.at("error") == "Invalid JSON body");
}

TEST_CASE("serve API missing message returns 400", "[serve][api]") {
  ServeHarness harness(DefaultResponder);

  boost::json::value req = {
    {"request_id", "bad-1"}
  };
  auto res = harness.Post("/api/chat", req);
  REQUIRE(res);
  REQUIRE(res->status == 400);
  auto body = ParseJson(res->body);
  REQUIRE(body.at("success") == false);
  REQUIRE(body.at("error") == "Missing or invalid 'message' field");
}
