// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/app/cli.hpp"
#include "pu/runtime.hpp"
#include "pu/core/platform.hpp"
#include "pu/session/session.hpp"
#include "tests/mocks/serve_harness.hpp"
#include "tests/mocks/test_helpers.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

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

TEST_CASE("serve API /api/session names the backend it is talking to", "[serve][api]") {
  ServeHarness harness("codebuddy");
  auto client = harness.Client();

  auto j = ParseJson(client.Get("/api/session"));

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("backend_type") == "codebuddy");
  REQUIRE(j.at("backend_model") == "test-model");
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
  ServeHarness harness("openai");
  auto client = harness.Client();

  auto session_j = ParseJson(client.Get("/api/session"));
  REQUIRE(session_j.at("thinking") == "default");
  REQUIRE(session_j.at("supports_thinking_level") == true);

  auto set_j = ParseJson(client.Post("/api/thinking", boost::json::object{{"level", "high"}}));
  REQUIRE(set_j.at("success") == true);
  REQUIRE(set_j.at("thinking") == "high");
  REQUIRE(set_j.at("thinking_override") == "high");

  auto after_j = ParseJson(client.Get("/api/session"));
  REQUIRE(after_j.at("thinking") == "high");
  REQUIRE(after_j.at("thinking_override") == "high");

  auto auto_j = ParseJson(client.Post("/api/thinking", boost::json::object{{"level", "auto"}}));
  REQUIRE(auto_j.at("success") == true);
  REQUIRE(auto_j.at("thinking") == "default");
  REQUIRE_FALSE(auto_j.as_object().count("thinking_override") == 1);
}

TEST_CASE("serve API /api/thinking refuses what the backend cannot carry", "[serve][api]") {
  ServeHarness harness;  // ollama, where the model decides for itself
  auto client = harness.Client();

  auto refused = client.PostFull("/api/thinking", boost::json::object{{"level", "high"}});
  REQUIRE(refused.status == 400);
  REQUIRE(ParseJson(refused.body).at("success") == false);

  auto unknown = client.PostFull("/api/thinking", boost::json::object{{"level", "enormous"}});
  REQUIRE(unknown.status == 400);
  REQUIRE(ParseJson(unknown.body).at("success") == false);

  auto missing = client.PostFull("/api/thinking", boost::json::object{});
  REQUIRE(missing.status == 400);
  REQUIRE(ParseJson(missing.body).at("error") == "Missing or invalid 'level'");

  auto malformed = client.PostRaw("/api/thinking", "not json");
  REQUIRE(malformed.status == 400);
  REQUIRE(ParseJson(malformed.body).at("error") == "Invalid JSON");
}

TEST_CASE("serve API invalid JSON returns 400", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto res = client.PostRaw("/api/agent/switch", "not json");

  REQUIRE(res.status == 400);
  auto j = ParseJson(res.body);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "Invalid JSON");
}

TEST_CASE("serve API /api/rewind steps the session back", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.Runtime().GetOrCreateDefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->GetConversation();
  conversation.Append("user", "one");
  conversation.Append("assistant", "two");
  conversation.Append("user", "three");

  auto stepped = client.PostFull("/api/rewind", boost::json::object{{"turn", 3}});
  REQUIRE(stepped.status == 200);
  REQUIRE(ParseJson(stepped.body).at("success") == true);

  auto history = ParseJson(client.Get("/api/history"));
  REQUIRE(history.as_array().size() == 2);
  REQUIRE(history.as_array()[1].at("content") == "two");
}

TEST_CASE("serve API /api/rewind refuses a turn that is not there", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.Runtime().GetOrCreateDefaultSession();
  REQUIRE(session != nullptr);
  session->GetConversation().Append("user", "one");

  auto missing = client.PostFull("/api/rewind", boost::json::object{});
  REQUIRE(missing.status == 400);
  REQUIRE(ParseJson(missing.body).at("error") == "Missing or invalid 'turn'");

  auto past_end = client.PostFull("/api/rewind", boost::json::object{{"turn", 9}});
  REQUIRE(past_end.status == 400);
  REQUIRE(ParseJson(past_end.body).at("error") == "No such turn");

  auto malformed = client.PostRaw("/api/rewind", "not json");
  REQUIRE(malformed.status == 400);
  REQUIRE(ParseJson(malformed.body).at("error") == "Invalid JSON");

  REQUIRE(ParseJson(client.Get("/api/history")).as_array().size() == 1);
}

TEST_CASE("serve API /api/rewind refuses while a tool call is pending", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.Runtime().GetOrCreateDefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->GetConversation();
  conversation.Append("user", "one");

  pu::ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}}])");
  conversation.Append(assistant);
  REQUIRE(conversation.HasPendingToolCalls());

  auto refused = client.PostFull("/api/rewind", boost::json::object{{"turn", 1}});

  REQUIRE(refused.status == 400);
  REQUIRE(ParseJson(refused.body).at("success") == false);
}

TEST_CASE("serve API /api/workspaces lists only directories it can serve", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  const fs::path stranger = fs::temp_directory_path() /
                            (harness.Home().filename().string() + "_stranger");
  fs::create_directories(stranger);

  auto workspaces = ParseJson(client.Get("/api/workspaces")).at("workspaces").as_array();
  bool listed_own = false;
  bool listed_stranger = false;
  for (const auto& workspace : workspaces) {
    const fs::path path{std::string(workspace.at("path").as_string())};
    if (path == harness.Home()) listed_own = true;
    if (path == fs::absolute(stranger)) listed_stranger = true;
  }

  fs::remove_all(stranger);

  REQUIRE(listed_own);
  REQUIRE_FALSE(listed_stranger);
}
