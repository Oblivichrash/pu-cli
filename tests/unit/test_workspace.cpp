// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/text.hpp"
#include "pu/session/session.hpp"
#include <boost/json.hpp>

using namespace pu;

TEST_CASE("Workspace basic operations", "[workspace]") {
  Workspace ctx;
  ctx.Append("user", "Hello");
  ctx.Append("assistant", "Hi there!");

  REQUIRE(ctx.HistorySize() == 2);
  auto history = ctx.GetHistory();
  REQUIRE(history.size() == 2);
  REQUIRE(history[1].role == "assistant");
}

TEST_CASE("Workspace serialization round-trips", "[transcript]") {
  Workspace ws;
  ws.Append("user", "hello");

  const boost::json::value saved = ws.Serialize();
  auto restored = Workspace::Deserialize(saved);

  REQUIRE(restored->HistorySize() == 1);
}

TEST_CASE("Session serialization round-trips", "[transcript]") {
  Session session;
  config::BackendConfig backend;
  backend.type = config::BackendType::kOllama;
  backend.host = "http://127.0.0.1:11434";
  backend.model = "llama3.2:1b";
  backend.temperature = 0.7f;
  session.SetAgent("chat");
  session.SetBackendOverride(backend);
  session.GetWorkspace().Append("user", "hello");

  const boost::json::value saved = session.Serialize();
  const std::string written = json::PrettyPrint(saved);
  const boost::json::value reparsed = boost::json::parse(written);

  auto restored = Session::Deserialize(reparsed);
  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetWorkspace().HistorySize() == 1);
  REQUIRE(restored->GetWorkspace().GetHistory()[0].content == "hello");
  REQUIRE(restored->GetRuntimeSpec().backend_override.has_value());
  REQUIRE(restored->GetRuntimeSpec().backend_override->model == "llama3.2:1b");
}

TEST_CASE("A message holding invalid UTF-8 survives a save and load", "[transcript]") {
  Session session;
  // Bytes a localized library error carries: cp936 for two CJK characters,
  // which is what a Boost.Asio failure message contains on a Chinese Windows.
  session.GetWorkspace().Append("assistant", "Request failed: \xB2\xBB\xCA\xC7");

  const std::string written = json::PrettyPrint(session.Serialize());
  REQUIRE(text::IsValidUtf8(written));

  auto restored = Session::Deserialize(boost::json::parse(written));
  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetWorkspace().HistorySize() == 1);
}

TEST_CASE("Transcript round-trips tool calls as a JSON array", "[transcript]") {
  Transcript t;
  ChatMessage asst;
  asst.id = 1;
  asst.role = "assistant";
  asst.tool_calls =
      boost::json::parse(R"([{"id":"call_1","function":{"name":"ls","arguments":{"path":"."}}}])");
  t.Append(asst);

  auto restored = Transcript{};
  REQUIRE(Transcript::Deserialize(t.Serialize(), restored));
  auto h = restored.GetHistory();
  REQUIRE(h.size() == 1);
  REQUIRE(h[0].HasToolCalls());
  REQUIRE(h[0].tool_calls.as_array()[0].at("id") == "call_1");
  REQUIRE(restored.HasPendingToolCalls());
}

TEST_CASE("A tool result clears the pending tool call", "[transcript]") {
  Transcript t;
  ChatMessage asst;
  asst.role = "assistant";
  asst.tool_calls =
      boost::json::parse(R"([{"id":"call_1","function":{"name":"ls","arguments":{}}}])");
  t.Append(asst);
  REQUIRE(t.HasPendingToolCalls());

  ChatMessage tool;
  tool.role = "tool";
  tool.tool_name = "ls";
  tool.tool_call_id = "call_1";
  tool.content = "done";
  t.Append(tool);

  REQUIRE_FALSE(t.HasPendingToolCalls());
  auto h = t.GetHistory();
  REQUIRE(h.size() == 2);
  REQUIRE(h[1].tool_name == "ls");
  REQUIRE(h[1].tool_call_id == "call_1");
}

TEST_CASE("Serialization is stable across repeated round trips", "[transcript]") {
  Transcript t;
  ChatMessage user;
  user.role = "user";
  user.timestamp = "2026-09-21T10:00:00Z";
  user.content = "hello";
  t.Append(user);

  ChatMessage asst;
  asst.role = "assistant";
  asst.content = "checking";
  asst.reasoning_content = "because";
  asst.tool_calls =
      boost::json::parse(R"([{"id":"call_9","function":{"name":"ls","arguments":{"path":"."}}}])");
  t.Append(asst);

  ChatMessage tool;
  tool.role = "tool";
  tool.tool_name = "ls";
  tool.tool_call_id = "call_9";
  tool.content = R"({"success":true})";
  t.Append(tool);

  const std::string first = boost::json::serialize(t.Serialize());

  Transcript once;
  REQUIRE(Transcript::Deserialize(t.Serialize(), once));
  const std::string second = boost::json::serialize(once.Serialize());

  Transcript twice;
  REQUIRE(Transcript::Deserialize(boost::json::parse(second), twice));
  const std::string third = boost::json::serialize(twice.Serialize());

  REQUIRE(second == first);
  REQUIRE(third == first);

  // Storage is an object with nodes and a leaf, not the list the old layout used.
  const boost::json::value stored = boost::json::parse(first);
  REQUIRE(stored.is_object());
  REQUIRE(stored.at("nodes").as_array().size() == 3);
  REQUIRE(stored.at("leaf").is_string());
}

TEST_CASE("Appending continues from the leaf without dropping anything", "[transcript]") {
  Transcript t;
  for (int i = 1; i <= 20; ++i) {
    ChatMessage msg;
    msg.role = "user";
    msg.content = "msg" + std::to_string(i);
    t.Append(msg);
  }

  ChatMessage after;
  after.role = "user";
  after.content = "after";
  t.Append(after);

  auto h = t.GetHistory();
  REQUIRE(t.Size() == 21);
  REQUIRE(h.size() == 21);
  REQUIRE(h[0].content == "msg1");
  REQUIRE(h[19].content == "msg20");
  REQUIRE(h[20].content == "after");
}
