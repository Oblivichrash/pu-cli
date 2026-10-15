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
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using pu::tests::HttpResponse;
using pu::tests::ScopedEnvVar;
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

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
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
      {"success", true}, {"blocked", false}, {"output", "hi\n"}, {"error", ""}, {"exit_code", 0}});
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

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
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

TEST_CASE("serve API /api/thinking takes a level on every backend", "[serve][api]") {
  ServeHarness harness;  // ollama, which carries the level as a top-level `think`
  auto client = harness.Client();

  auto accepted = client.PostFull("/api/thinking", boost::json::object{{"level", "high"}});
  REQUIRE(accepted.status == 200);
  REQUIRE(ParseJson(accepted.body).at("thinking") == "high");

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

TEST_CASE("serve API switching to an agent that does not exist returns 404", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto res = client.PostFull("/api/agent/switch", boost::json::object{{"agent_name", "ghost"}});

  REQUIRE(res.status == 404);
  auto j = ParseJson(res.body);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "Agent not found: ghost");
}

TEST_CASE("serve API /api/rewind steps the session back", "[serve][api]") {
  ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
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

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  session->Messages().Append("user", "one");

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

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::Conversation& conversation = session->Messages();
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

  // The scan reads the harness home's parent, so the neighbours go there too. They are named
  // after this harness and removed below, so a parallel case's own neighbours stay untouched.
  const fs::path parent = harness.Home().parent_path();
  const std::string tag = harness.Home().filename().string();
  const fs::path stranger = parent / (tag + "_stranger");
  const fs::path sibling = parent / (tag + "_sibling");
  std::error_code ec;
  fs::create_directories(stranger, ec);
  REQUIRE_FALSE(ec);
  fs::create_directories(sibling / ".pu", ec);
  REQUIRE_FALSE(ec);
  {
    std::ofstream config(sibling / ".pu" / "agents.json", std::ios::binary | std::ios::trunc);
    REQUIRE(config.is_open());
    config << R"({"agents":[],"default":"chat"})";
  }

  const auto workspaces = ParseJson(client.Get("/api/workspaces")).at("workspaces").as_array();
  bool listed_own = false;
  bool listed_stranger = false;
  bool listed_sibling = false;
  for (const auto& workspace : workspaces) {
    const fs::path path{std::string(workspace.at("path").as_string())};
    // The API answers with absolute paths, and on a platform where /tmp is a symlink the
    // harness home and the answer differ before canonicalization. Compare canonically.
    if (fs::weakly_canonical(path) == fs::weakly_canonical(harness.Home())) listed_own = true;
    if (fs::weakly_canonical(path) == fs::weakly_canonical(stranger)) listed_stranger = true;
    if (fs::weakly_canonical(path) == fs::weakly_canonical(sibling)) listed_sibling = true;
  }

  fs::remove_all(stranger, ec);
  fs::remove_all(sibling, ec);

  CHECK(listed_own);
  CHECK(listed_sibling);
  CHECK_FALSE(listed_stranger);
}

TEST_CASE("serve page files are mounted and served from the web directory", "[serve][page]") {
  const fs::path web = fs::temp_directory_path() / "pu_serve_web_fixture";
  std::error_code ec;
  fs::remove_all(web, ec);
  fs::create_directories(web, ec);
  REQUIRE_FALSE(ec);

  const auto write_page = [&](const std::string& name, const std::string& body) {
    std::ofstream file(web / name, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file << body;
    REQUIRE(file.good());
  };
  write_page("index.html", "<!doctype html>fixture index");
  write_page("style.css", ":root{--column:720px}");
  write_page("app.js", "const APP = true;");

  const ScopedEnvVar web_dir("PU_WEB_DIR", web.string());

  ServeHarness harness;
  auto client = harness.Client();

  const HttpResponse root = client.GetRaw("/");
  CHECK(root.status == 200);
  CHECK(root.body == "<!doctype html>fixture index");

  const HttpResponse explicit_index = client.GetRaw("/index.html");
  CHECK(explicit_index.status == 200);
  CHECK(explicit_index.body == "<!doctype html>fixture index");

  const HttpResponse css = client.GetRaw("/style.css");
  CHECK(css.status == 200);
  CHECK(css.body == ":root{--column:720px}");

  const HttpResponse script = client.GetRaw("/app.js");
  CHECK(script.status == 200);
  CHECK(script.body == "const APP = true;");

  const HttpResponse unmounted = client.GetRaw("/secret.txt");
  CHECK(unmounted.status == 404);

  fs::remove_all(web, ec);
}
