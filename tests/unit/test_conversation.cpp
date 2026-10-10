// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/text.hpp"
#include "pu/session/session.hpp"
#include <boost/json.hpp>

using namespace pu;

TEST_CASE("Conversation basic operations", "[conversation]") {
  Conversation ctx;
  ctx.Append("user", "Hello");
  ctx.Append("assistant", "Hi there!");

  REQUIRE(ctx.GetHistory().size() == 2);
  auto history = ctx.GetHistory();
  REQUIRE(history.size() == 2);
  REQUIRE(history[1].role == "assistant");
}

TEST_CASE("Conversation serialization round-trips", "[conversation]") {
  Conversation ws;
  ws.Append("user", "hello");

  const boost::json::value saved = ws.Serialize();
  auto restored = Conversation::Deserialize(saved);

  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetHistory().size() == 1);
}

TEST_CASE("Session serialization round-trips", "[conversation]") {
  Session session;
  session.SetAgent("chat");
  session.GetConversation().Append("user", "hello");

  const boost::json::value saved = session.Serialize();
  const std::string written = json::PrettyPrint(saved);
  const boost::json::value reparsed = boost::json::parse(written);

  auto restored = Session::Deserialize(reparsed);
  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetConversation().GetHistory().size() == 1);
  REQUIRE(restored->GetConversation().GetHistory()[0].content == "hello");
  REQUIRE(restored->GetSpec().agent_name == "chat");
}

TEST_CASE("A message holding invalid UTF-8 survives a save and load", "[conversation]") {
  Session session;
  session.GetConversation().Append("assistant", "Request failed: \xB2\xBB\xCA\xC7");

  const std::string written = json::PrettyPrint(session.Serialize());
  REQUIRE(text::IsValidUtf8(written));

  auto restored = Session::Deserialize(boost::json::parse(written));
  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetConversation().GetHistory().size() == 1);
}

TEST_CASE("Tool calls round-trip as a JSON array", "[conversation]") {
  Conversation t;
  ChatMessage asst;
  asst.id = 1;
  asst.role = "assistant";
  asst.tool_calls =
      boost::json::parse(R"([{"id":"call_1","function":{"name":"ls","arguments":{"path":"."}}}])");
  t.Append(asst);

  auto restored = Conversation::Deserialize(t.Serialize());
  REQUIRE(restored != nullptr);
  auto h = restored->GetHistory();
  REQUIRE(h.size() == 1);
  REQUIRE(h[0].HasToolCalls());
  REQUIRE(h[0].tool_calls.as_array()[0].at("id") == "call_1");
  REQUIRE(restored->HasPendingToolCalls());
}

TEST_CASE("A tool result clears the pending tool call", "[conversation]") {
  Conversation t;
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

TEST_CASE("Serialization is stable across repeated round trips", "[conversation]") {
  Conversation t;
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

  auto once = Conversation::Deserialize(t.Serialize());
  REQUIRE(once != nullptr);
  const std::string second = boost::json::serialize(once->Serialize());

  auto twice = Conversation::Deserialize(boost::json::parse(second));
  REQUIRE(twice != nullptr);
  const std::string third = boost::json::serialize(twice->Serialize());

  REQUIRE(second == first);
  REQUIRE(third == first);

  const boost::json::value stored = boost::json::parse(first);
  REQUIRE(stored.is_object());
  REQUIRE(stored.at("history").at("nodes").as_array().size() == 3);
  REQUIRE(stored.at("history").at("leaf").is_string());
}

TEST_CASE("Appending continues from the leaf without dropping anything", "[conversation]") {
  Conversation t;
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
  REQUIRE(h.size() == 21);
  REQUIRE(h[0].content == "msg1");
  REQUIRE(h[19].content == "msg20");
  REQUIRE(h[20].content == "after");
}
