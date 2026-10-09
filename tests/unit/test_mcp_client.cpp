// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/core/json.hpp"
#include "pu/mcp/client.hpp"
#include "tests/mocks/serve_harness.hpp"

#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <future>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;

using namespace std::chrono_literals;

namespace {

// A transport the test drives by hand: it records what the client wrote and hands a message
// back when the case says so, which is how the pairing rules are observed without a server or
// a child process.
class FakeTransport : public pu::mcp::Transport {
 public:
  bool Start(pu::mcp::MessageCallback on_message) override {
    on_message_ = std::move(on_message);
    started_ = true;
    return true;
  }

  void Stop() override { started_ = false; }

  bool WriteLine(const std::string& line) override {
    if (!started_ || failing_) return false;
    written_.push_back(line);
    return true;
  }

  void FailWrites() { failing_ = true; }

  const std::vector<std::string>& Written() const { return written_; }

  // What a server would send: the callback the transport was started with.
  void Deliver(const std::string& line) {
    REQUIRE(on_message_);
    on_message_(line);
  }

 private:
  pu::mcp::MessageCallback on_message_;
  std::vector<std::string> written_;
  bool started_ = false;
  bool failing_ = false;
};

int RequestId(const std::string& line) {
  return boost::json::value_to<int>(boost::json::parse(line).at("id"));
}

std::string Reply(int id, boost::json::value result) {
  return boost::json::serialize(
      boost::json::value{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

std::string ErrorReply(int id) {
  return boost::json::serialize(
      boost::json::value{{"jsonrpc", "2.0"},
                         {"id", id},
                         {"error", {{"code", -32601}, {"message", "no such method"}}}});
}

struct Rpc {
  FakeTransport transport;
  pu::mcp::JsonRpcClient rpc;

  Rpc() : rpc(transport) {
    REQUIRE(transport.Start([this](const std::string& line) { rpc.OnMessage(line); }));
  }

  int LastId() const { return RequestId(transport.Written().back()); }
};

http::response<http::string_body> Answer(unsigned status, std::string body) {
  http::response<http::string_body> res;
  res.result(static_cast<http::status>(status));
  res.set(http::field::content_type, "application/json");
  res.body() = std::move(body);
  res.prepare_payload();
  return res;
}

// An MCP server reached over HTTP. It answers a request body with a reply for the method it
// carries, in a plain JSON body or in one `data:` frame, and refuses a call when asked to.
class McpServer {
 public:
  struct Options {
    bool as_sse = false;
    bool refuse_a_call = false;
    bool refuse_the_handshake = false;
  };

  explicit McpServer(Options options) : options_(options) {}

  http::response<http::string_body> operator()(const http::request<http::string_body>& req) {
    const auto body = boost::json::parse(req.body());
    if (!pu::json::HasKey(body, "id")) return Answer(200, "");  // the `initialized` notification

    const int id = boost::json::value_to<int>(body.at("id"));
    const std::string method = boost::json::value_to<std::string>(body.at("method"));

    if (method == "tools/call" && options_.refuse_a_call) return Wrap(ErrorReply(id));
    if (method == "initialize" && options_.refuse_the_handshake) {
      return Wrap(boost::json::serialize(boost::json::value{{"jsonrpc", "2.0"}, {"id", id}}));
    }

    boost::json::object reply = {{"jsonrpc", "2.0"}, {"id", id}};
    if (method == "initialize") {
      reply["result"] = {{"protocolVersion", "2024-11-05"}};
    } else if (method == "tools/list") {
      reply["result"] = {
          {"tools",
           boost::json::array{
               boost::json::value{{"name", "echo"},
                                  {"description", "Echoes what it is given"},
                                  {"inputSchema", {{"type", "object"}}}},
               boost::json::value{{"name", "count"}, {"description", "Counts something"}}}}};
    } else if (method == "tools/call") {
      reply["result"] = {
          {"content", boost::json::array{boost::json::value{{"type", "text"}, {"text", "one"}},
                                         boost::json::value{{"type", "text"}, {"text", "two"}}}}};
    } else {
      return Wrap(ErrorReply(id));
    }
    return Wrap(boost::json::serialize(reply));
  }

  pu::tests::FakeHttpServer::Responder ToResponder() {
    return [this](const http::request<http::string_body>& req) { return (*this)(req); };
  }

 private:
  http::response<http::string_body> Wrap(const std::string& json) const {
    return Answer(200, options_.as_sse ? "data: " + json + "\n\n" : json);
  }

  Options options_;
};

pu::mcp::McpServerConfig ServerConfigAt(int port, std::map<std::string, std::string> headers = {}) {
  pu::mcp::McpServerConfig config;
  config.name = "fake";
  config.url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
  config.headers = std::move(headers);
  return config;
}

// The notification the handshake sends is a request of its own, so waiting for a count is how
// a case knows the server has seen it.
void WaitForRequests(const pu::tests::FakeHttpServer& server, int wanted) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (server.Requests() >= wanted) return;
    std::this_thread::sleep_for(5ms);
  }
  FAIL("the server never saw " << wanted << " requests");
}

}  // namespace

TEST_CASE("A request is answered by the reply carrying its id", "[mcp][jsonrpc]") {
  Rpc rpc;

  auto future = rpc.rpc.SendRequest("tools/list");
  REQUIRE(rpc.transport.Written().size() == 1);

  const auto request = boost::json::parse(rpc.transport.Written().front());
  REQUIRE(boost::json::value_to<std::string>(request.at("jsonrpc")) == "2.0");
  REQUIRE(boost::json::value_to<std::string>(request.at("method")) == "tools/list");

  rpc.transport.Deliver(
      Reply(RequestId(rpc.transport.Written().front()), {{"tools", boost::json::array{}}}));

  REQUIRE(future.wait_for(0ms) == std::future_status::ready);
  REQUIRE(future.get().at("result").at("tools").as_array().empty());
}

TEST_CASE("Replies are paired by id, not by the order they arrive in", "[mcp][jsonrpc]") {
  Rpc rpc;

  auto first = rpc.rpc.SendRequest("first");
  auto second = rpc.rpc.SendRequest("second");
  const int first_id = RequestId(rpc.transport.Written()[0]);
  const int second_id = RequestId(rpc.transport.Written()[1]);
  REQUIRE(first_id != second_id);

  // The answer to the second request arrives first.
  rpc.transport.Deliver(Reply(second_id, "for the second"));
  rpc.transport.Deliver(Reply(first_id, "for the first"));

  REQUIRE(boost::json::value_to<std::string>(second.get().at("result")) == "for the second");
  REQUIRE(boost::json::value_to<std::string>(first.get().at("result")) == "for the first");
}

TEST_CASE("An error reply fails the request with what the server said", "[mcp][jsonrpc]") {
  Rpc rpc;

  auto future = rpc.rpc.SendRequest("tools/call");
  rpc.transport.Deliver(ErrorReply(rpc.LastId()));

  REQUIRE(future.wait_for(0ms) == std::future_status::ready);
  try {
    future.get();
    FAIL("expected the request to fail");
  } catch (const std::runtime_error& e) {
    REQUIRE(std::string(e.what()).find("no such method") != std::string::npos);
  }
}

TEST_CASE("A notification and a line that is not JSON are both ignored", "[mcp][jsonrpc]") {
  Rpc rpc;

  // A notification carries no id, so there is nothing to answer.
  rpc.transport.Deliver(R"({"jsonrpc":"2.0","method":"notifications/message"})");
  rpc.transport.Deliver("this is not json");

  auto future = rpc.rpc.SendRequest("after");
  rpc.transport.Deliver(Reply(rpc.LastId(), "still works"));
  REQUIRE(boost::json::value_to<std::string>(future.get().at("result")) == "still works");
}

TEST_CASE("A write that fails fails the request", "[mcp][jsonrpc]") {
  Rpc rpc;
  rpc.transport.FailWrites();

  auto future = rpc.rpc.SendRequest("tools/list");

  REQUIRE(future.wait_for(0ms) == std::future_status::ready);
  REQUIRE_THROWS_AS(future.get(), std::runtime_error);
}

TEST_CASE("Connect shakes hands and lists the tools once", "[mcp][http]") {
  McpServer server({});
  pu::tests::FakeHttpServer http(server.ToResponder());

  pu::mcp::McpClient client(ServerConfigAt(http.Port()));
  REQUIRE(client.Connect());
  REQUIRE(client.IsConnected());
  WaitForRequests(http, 2);  // initialize, then the `initialized` notification

  const auto tools = client.ListTools();
  REQUIRE(tools.size() == 2);
  REQUIRE(tools[0].name == "echo");
  REQUIRE(tools[0].description == "Echoes what it is given");
  REQUIRE(tools[0].parameters.is_object());
  REQUIRE(tools[1].name == "count");
  // A tool without a schema is reported with an empty one rather than a null.
  REQUIRE(tools[1].parameters.as_object().empty());

  // The list is asked for once and kept.
  REQUIRE(client.ListTools().size() == 2);
  REQUIRE(http.Requests() == 3);

  client.Disconnect();
  REQUIRE_FALSE(client.IsConnected());
}

TEST_CASE("A handshake answered in an SSE frame is read the same way", "[mcp][http]") {
  McpServer server({.as_sse = true});
  pu::tests::FakeHttpServer http(server.ToResponder());

  pu::mcp::McpClient client(ServerConfigAt(http.Port()));
  REQUIRE(client.Connect());
  REQUIRE(client.ListTools().size() == 2);
}

TEST_CASE("A call answers with the text of every content item", "[mcp][http]") {
  McpServer server({});
  pu::tests::FakeHttpServer http(server.ToResponder());

  pu::mcp::McpClient client(ServerConfigAt(http.Port()));
  REQUIRE(client.Connect());

  REQUIRE(client.CallTool("echo", boost::json::object{}) == "one\ntwo\n");
}

TEST_CASE("A call the server refuses is reported as a failure", "[mcp][http]") {
  McpServer server({.refuse_a_call = true});
  pu::tests::FakeHttpServer http(server.ToResponder());

  pu::mcp::McpClient client(ServerConfigAt(http.Port()));
  REQUIRE(client.Connect());

  // The reply's error travels out of the JSON-RPC client as an exception, so the answer the
  // caller gets is the one that says the call failed.
  const std::string answer = client.CallTool("echo", boost::json::object{});
  REQUIRE(answer.starts_with("MCP call error:"));
  REQUIRE(answer.find("no such method") != std::string::npos);
}

TEST_CASE("A handshake the server refuses leaves the client unconnected", "[mcp][http]") {
  McpServer server({.refuse_the_handshake = true});
  pu::tests::FakeHttpServer http(server.ToResponder());

  pu::mcp::McpClient client(ServerConfigAt(http.Port()));
  REQUIRE_FALSE(client.Connect());
  REQUIRE_FALSE(client.IsConnected());
}

TEST_CASE("A client that never connected reports that on every call", "[mcp][http]") {
  McpServer server({});
  pu::tests::FakeHttpServer http(server.ToResponder());

  pu::mcp::McpClient client(ServerConfigAt(http.Port()));
  REQUIRE_FALSE(client.IsConnected());
  REQUIRE(client.ListTools().empty());
  REQUIRE(client.CallTool("echo", boost::json::object{})
              .starts_with("Error: MCP client not connected"));
}

TEST_CASE("A server that does not answer in time leaves the client unconnected", "[mcp][http]") {
  McpServer server({});
  // The client's request timeout is a fixed five seconds, so this case takes about that long.
  pu::tests::FakeHttpServer http(server.ToResponder(), 6000);

  pu::mcp::McpClient client(ServerConfigAt(http.Port()));
  REQUIRE_FALSE(client.Connect());
  REQUIRE_FALSE(client.IsConnected());
}
