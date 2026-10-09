// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/cli.hpp"
#include "pu/runtime.hpp"
#include "pu/core/platform.hpp"
#include "pu/session/session.hpp"
#include "tests/mocks/serve_harness.hpp"
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

// Boost.Beast aliases used throughout this test file.
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

static constexpr const char* kHost = "127.0.0.1";

using pu::tests::ServeHarness;
using pu::tests::TestHttpClient;

namespace {

boost::json::value ParseJson(const std::string& s) { return boost::json::parse(s); }

}  // namespace

TEST_CASE("serve API /api/session", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string body = client.Get("/api/session");
  auto j = ParseJson(body);

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("agent_name") == "chat");
  REQUIRE(j.at("backend_type") == "ollama");
  REQUIRE(j.at("backend_model") == "test-model");
}

// With a third type, answering "is it OpenAI?" turns every answer into "ollama" rather than
// into a failure, and the Web header showed the wrong backend for the one it talked to.
TEST_CASE("serve API /api/session names the backend it is talking to", "[serve][api]") {
  ServeHarness harness("codebuddy");
  auto client = harness.Client();

  auto j = ParseJson(client.Get("/api/session"));

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("backend_type") == "codebuddy");
  REQUIRE(j.at("backend_model") == "test-model");
  // A gateway that honours reasoning_effort carries the level control.
  REQUIRE(j.at("supports_thinking_level") == true);
}

TEST_CASE("serve API /api/history initially empty", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  std::string body = client.Get("/api/history");
  auto j = ParseJson(body);

  REQUIRE(j.is_array());
  REQUIRE(j.as_array().empty());
}

// The reload draws the conversation from this endpoint alone, so everything the streamed
// turn showed has to be in it: a tool result as the same output, a call with no result as such.
TEST_CASE("serve API /api/history says how each tool call ended", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.Runtime().GetOrCreateDefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->GetConversation();
  conversation.Append("user", "run it");

  pu::ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::array{
      boost::json::object{
          {"id", "call_1"},
          {"type", "function"},
          {"function",
           boost::json::object{{"name", "execute_bash"},
                               {"arguments", boost::json::object{{"command", "echo hi"}}}}}},
      boost::json::object{
          {"id", "call_2"},
          {"type", "function"},
          {"function",
           boost::json::object{{"name", "execute_bash"},
                               {"arguments", boost::json::object{{"command", "echo there"}}}}}},
  };
  conversation.Append(assistant);

  // One call is answered, the other is left as the store keeps it when a turn ends
  // between a call and its result.
  pu::ChatMessage receipt;
  receipt.role = "tool";
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "execute_bash";
  receipt.content = boost::json::serialize(boost::json::object{
      {"success", true}, {"stdout", "hi\n"}, {"stderr", ""}, {"error", ""}, {"exit_code", 0}});
  conversation.Append(receipt);

  auto j = ParseJson(client.Get("/api/history"));
  REQUIRE(j.as_array().size() == 3);

  const boost::json::value& call_turn = j.as_array()[1];
  REQUIRE(call_turn.at("tool_calls").as_array().size() == 2);
  REQUIRE(call_turn.at("tool_call_status") == boost::json::array{"done", "pending"});

  // What the tool printed, not the envelope it was wrapped in.
  const boost::json::value& result_turn = j.as_array()[2];
  REQUIRE(result_turn.at("output") == "hi\n");
  REQUIRE(result_turn.at("error") == "");
}

TEST_CASE("serve API /api/history keeps a result it cannot parse", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.Runtime().GetOrCreateDefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->GetConversation();
  conversation.Append("user", "run it");

  pu::ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::array{boost::json::object{
      {"id", "call_1"},
      {"type", "function"},
      {"function", boost::json::object{{"name", "execute_bash"},
                                       {"arguments", boost::json::object{{"command", "ls"}}}}}}};
  conversation.Append(assistant);

  // A built-in or MCP tool answers in its own words, which is output too.
  pu::ChatMessage receipt;
  receipt.role = "tool";
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "execute_bash";
  receipt.content = "plain text from a tool";
  conversation.Append(receipt);

  auto j = ParseJson(client.Get("/api/history"));
  REQUIRE(j.as_array().size() == 3);
  REQUIRE(j.as_array()[2].at("output") == "plain text from a tool");
  REQUIRE(j.as_array()[2].at("error") == "");
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

TEST_CASE("serve API /api/thinking", "[serve][api]") {
  // An OpenAI-compatible backend is where a level lands, so this is where the
  // control has something to set.
  ServeHarness harness("openai");
  auto client = harness.Client();

  auto session_j = ParseJson(client.Get("/api/session"));
  REQUIRE(session_j.at("thinking") == "default");
  REQUIRE(session_j.at("supports_thinking_level") == true);

  auto set_j = ParseJson(client.Post("/api/thinking", boost::json::object{{"level", "high"}}));
  REQUIRE(set_j.at("success") == true);
  // The session's own level is what the next request carries, and the session
  // reports it as its own rather than as the configuration's.
  REQUIRE(set_j.at("thinking") == "high");
  REQUIRE(set_j.at("thinking_override") == "high");

  auto after_j = ParseJson(client.Get("/api/session"));
  REQUIRE(after_j.at("thinking") == "high");
  REQUIRE(after_j.at("thinking_override") == "high");

  // `auto` hands the choice back to the agent's configuration.
  auto auto_j = ParseJson(client.Post("/api/thinking", boost::json::object{{"level", "auto"}}));
  REQUIRE(auto_j.at("success") == true);
  REQUIRE(auto_j.at("thinking") == "default");
  REQUIRE_FALSE(auto_j.as_object().count("thinking_override") == 1);
}

TEST_CASE("serve API /api/thinking refuses what the backend cannot carry", "[serve][api]") {
  ServeHarness harness;  // ollama, where the model decides for itself
  auto client = harness.Client();

  auto refused_j = ParseJson(client.Post("/api/thinking", boost::json::object{{"level", "high"}}));
  REQUIRE(refused_j.at("success") == false);

  // A word that names no level is refused rather than read as some default.
  auto unknown_j =
      ParseJson(client.Post("/api/thinking", boost::json::object{{"level", "enormous"}}));
  REQUIRE(unknown_j.at("success") == false);
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

    beast::error_code close_ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, close_ec);
  } catch (const std::exception& e) {
    FAIL("HTTP request failed: " << e.what());
  }
}
