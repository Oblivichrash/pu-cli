// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Two `pu serve` instances, each with a backend that gives a chosen answer.
//
// The peer protocol is one HTTP hop in each direction, so nothing between the two servers is
// a mock: A's turn calls send_to_peer, the request reaches B's /api/peer/message, and B
// answers in a turn of its own. What a case picks is what each backend says.

#include "tests/mocks/serve_harness.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace pu::tests {

inline constexpr const char* kPeerToken = "shared-secret";

// The .pu/peers.json a server finds in its working directory. It is read once at startup, so
// it has to be there before the server binds.
inline void WritePeersFile(const std::filesystem::path& dir, const std::string& self_name,
                           int self_port, const std::string& peer_name, int peer_port,
                           const std::string& token = kPeerToken) {
  std::error_code ec;
  std::filesystem::create_directories(dir / ".pu", ec);
  REQUIRE_FALSE(ec);

  boost::json::object self;
  self["name"] = JsonStr(self_name);
  self["host"] = JsonStr(kServeHost);
  self["port"] = self_port;

  boost::json::object peer;
  peer["name"] = JsonStr(peer_name);
  peer["url"] = JsonStr("http://" + std::string(kServeHost) + ":" + std::to_string(peer_port));

  boost::json::object root;
  root["self"] = std::move(self);
  root["token"] = JsonStr(token);
  root["peers"] = boost::json::array{std::move(peer)};

  std::ofstream file(dir / ".pu" / "peers.json", std::ios::binary | std::ios::trunc);
  REQUIRE(file.is_open());
  file << boost::json::serialize(root);
  REQUIRE(file.good());
}

// An agents.json whose one agent talks to a FakeBackend over the OpenAI protocol, which is the
// one that carries a tool call as JSON — so a backend can be made to call send_to_peer by
// answering with one.
inline void WritePeerBackendConfig(const std::filesystem::path& dir, int backend_port) {
  std::error_code ec;
  std::filesystem::create_directories(dir / ".pu", ec);
  REQUIRE_FALSE(ec);

  boost::json::object backend;
  backend["gateway"] = JsonStr("openai");
  backend["host"] = JsonStr("http://127.0.0.1:" + std::to_string(backend_port));
  backend["model"] = JsonStr("test-model");

  boost::json::object security;
  security["sandbox_root"] = JsonStr(".");
  security["forbidden_patterns"] = boost::json::array{};

  boost::json::object agent;
  agent["name"] = JsonStr("chat");
  agent["description"] = JsonStr("Peer agent");
  agent["backend"] = std::move(backend);
  agent["security"] = std::move(security);

  boost::json::object root;
  root["default_agent"] = JsonStr("chat");
  root["agents"] = boost::json::array{std::move(agent)};

  std::ofstream file(dir / ".pu" / "agents.json", std::ios::binary | std::ios::trunc);
  REQUIRE(file.is_open());
  file << boost::json::serialize(root);
  REQUIRE(file.good());
}

// One streamed OpenAI answer, which is what a turn reads. `piece` is the delta object of the
// single choice, so a caller says what the model said and nothing about framing; `finish` is
// "tool_calls" for a turn that calls a tool and "stop" for one that answers.
inline std::string OpenAiStream(const boost::json::object& piece, const std::string& finish) {
  boost::json::object choice;
  choice["index"] = 0;
  choice["delta"] = piece;
  choice["finish_reason"] = finish.empty() ? boost::json::value() : boost::json::value(finish);

  boost::json::object frame;
  frame["model"] = JsonStr("test-model");
  frame["choices"] = boost::json::array{std::move(choice)};

  boost::json::object usage;
  usage["prompt_tokens"] = 11;
  usage["completion_tokens"] = 3;
  usage["total_tokens"] = 14;

  boost::json::object tail;
  tail["model"] = JsonStr("test-model");
  tail["choices"] = boost::json::array{};
  tail["usage"] = std::move(usage);

  // A reply's last frame may carry `usage` and still lack the content fields the request
  // expects, so the same object is added to the frame that ends the choice: the protocol
  // treats a missing usage as a malformed reply rather than as a reply that cost nothing.
  frame["usage"] = usage;

  return "data: " + boost::json::serialize(frame) + "\n\ndata: " + boost::json::serialize(tail) +
         "\n\ndata: [DONE]\n\n";
}

// One side of a topology: a workspace with its own peers.json, a backend that gives a chosen
// answer, and a listener the other side can reach.
//
// The shutdown token is process-wide, so only one server can be running at a time here. A case
// that wants both ends alive starts both, does its work, and stops both.
class PeerServer {
 public:
  // What one request is answered with, as the delta object of its single streamed choice and
  // the finish reason that matches it: {"content": "..."} with "stop" answers in words, and a
  // `tool_calls` delta with "tool_calls" is a turn that calls a tool.
  struct Reply {
    boost::json::object answer;
    std::string finish;
  };

  // A fixed reply, repeated for every request: what a server that only ever answers says.
  PeerServer(const std::string& name, const boost::json::object& answer, const std::string& finish)
      : PeerServer(name, [answer, finish](int) { return Reply{answer, finish}; }) {}

  // A reply that may differ per request: the first is the tool call, the second the answer the
  // model gives once the tool result is in the conversation.
  PeerServer(const std::string& name, std::function<Reply(int)> reply)
      : reply_(std::move(reply)) {
    static std::atomic<int> seq{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    home_ = UniqueTempPath("pu_peer_" + name + "_" + std::to_string(stamp) + "_" +
                           std::to_string(seq.fetch_add(1)));
    std::filesystem::create_directories(home_ / ".pu");

    backend_ = std::make_unique<FakeHttpServer>(
        [this](const http::request<http::string_body>& req) { return Answer(req); });
    WritePeerBackendConfig(home_, backend_->Port());

    port_ = FindFreePort();
    REQUIRE(port_ > 0);
  }

  PeerServer(const PeerServer&) = delete;
  PeerServer& operator=(const PeerServer&) = delete;
  ~PeerServer() { Stop(); }

  const std::filesystem::path& Home() const { return home_; }
  int Port() const { return port_; }
  TestHttpClient Client() const { return TestHttpClient(kServeHost, port_); }

  // The environment a server needs is set for the whole time it runs, since `Runtime` reads
  // HOME and PU_HOME lazily. Both are restored in `Stop`, which every exit path reaches: a
  // fixture that left them set would move the log directory of the next test, and a failed
  // REQUIRE out of a constructor would leak them for the rest of the binary.
  void Start() {
    home_env_ = std::make_unique<ScopedEnvVar>("HOME", home_.string());
    const std::filesystem::path data_dir = UniqueTempPath("pu_peer_data");
    std::filesystem::create_directories(data_dir);
    data_env_ = std::make_unique<ScopedEnvVar>("PU_HOME", data_dir.string());

    // The workspace is the working directory: both agents.json and peers.json are found
    // relative to it, and `./.pu` is what a deployment reads.
    {
      ScopedWorkingDir here(home_);
      runtime_ = std::make_unique<pu::Runtime>();
      runtime_->Initialize();
    }

    server_thread_ = std::thread(
        [this] { cli::RunServe(kServeHost, static_cast<std::uint16_t>(port_), *runtime_); });
    const bool listening = WaitForPort(kServeHost, port_, 15000);
    if (!listening) {
      Stop();
      FAIL("the server never took the port FindFreePort named");
    }
  }

  void Stop() {
    if (stopped_.exchange(true)) return;
    platform::ShutdownToken()->store(true, std::memory_order_release);
    if (server_thread_.joinable()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      server_thread_.join();
    }
    platform::ShutdownToken()->store(false, std::memory_order_release);
    if (runtime_) runtime_->Shutdown();
    // Restored after the runtime is gone: a server that outlived it would read the wrong
    // directory. `Stop` runs once, so this is reached on every exit path.
    data_env_.reset();
    home_env_.reset();
  }

  // The text of every turn in the store, in order. A turn that arrived from a peer has no page
  // to send frames to, so the session is where it can be read.
  std::string Transcript() const {
    std::string text;
    for (const auto& message : runtime_->DefaultSession()->Messages().History()) {
      text += message.role + ": " + message.content + "\n";
    }
    return text;
  }

  bool TranscriptContains(const std::string& needle) const {
    return Transcript().find(needle) != std::string::npos;
  }

  // Waits for the store to hold a turn, so a case reads a settled transcript rather than a race.
  bool WaitForTranscript(const std::string& needle, int timeout_ms = 10000) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (TranscriptContains(needle)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
  }

 private:
  http::response<http::string_body> Answer(const http::request<http::string_body>& req) {
    http::response<http::string_body> res;
    const bool chat =
        req.target() == "/v1/chat/completions" || req.target() == "/chat/completions";
    if (req.method() != http::verb::post || !chat) {
      res.result(http::status::not_found);
      res.prepare_payload();
      return res;
    }
    const Reply reply = reply_(++requests_);
    res.result(http::status::ok);
    res.set(http::field::content_type, "text/event-stream");
    res.body() = OpenAiStream(reply.answer, reply.finish);
    res.prepare_payload();
    return res;
  }

  std::filesystem::path home_;
  std::function<Reply(int)> reply_;
  int requests_ = 0;
  std::unique_ptr<FakeHttpServer> backend_;
  std::unique_ptr<ScopedEnvVar> home_env_;
  std::unique_ptr<ScopedEnvVar> data_env_;
  std::unique_ptr<pu::Runtime> runtime_;
  std::thread server_thread_;
  int port_ = 0;
  std::atomic<bool> stopped_{false};
};

}  // namespace pu::tests
