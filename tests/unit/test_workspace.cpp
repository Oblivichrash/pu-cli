// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/session/workspace.hpp"
#include "pu/session/memory.hpp"
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

  ctx.SetVar("foo", boost::json::value("bar"));
  auto val = ctx.GetVar("foo");
  REQUIRE(val.has_value());
  REQUIRE(boost::json::value_to<std::string>(*val) == "bar");
}
TEST_CASE("Artifact operations", "[workspace]") {
  Workspace ctx;
  Artifact f;
  f.type = Artifact::Type::kFilePath;
  f.content = "/tmp/data.csv";
  f.source = "user_input";
  ctx.AddArtifact(f);
  REQUIRE(ctx.GetArtifacts().size() == 1);
}

TEST_CASE("Transcript round-trips tool calls as a JSON array", "[transcript]") {
  Transcript t;
  ChatMessage asst;
  asst.id = 1;
  asst.role = "assistant";
  asst.tool_calls = boost::json::parse(
      R"([{"id":"call_1","function":{"name":"ls","arguments":{"path":"."}}}])");
  t.Append(asst);

  auto restored = Transcript::Deserialize(t.Serialize());
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
  asst.tool_calls = boost::json::parse(
      R"([{"id":"call_1","function":{"name":"ls","arguments":{}}}])");
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
  asst.tool_calls = boost::json::parse(
      R"([{"id":"call_9","function":{"name":"ls","arguments":{"path":"."}}}])");
  t.Append(asst);

  ChatMessage tool;
  tool.role = "tool";
  tool.tool_name = "ls";
  tool.tool_call_id = "call_9";
  tool.content = R"({"success":true})";
  t.Append(tool);

  const std::string first = boost::json::serialize(t.Serialize());
  const std::string second =
      boost::json::serialize(Transcript::Deserialize(t.Serialize()).Serialize());
  const std::string third =
      boost::json::serialize(Transcript::Deserialize(boost::json::parse(second)).Serialize());

  REQUIRE(second == first);
  REQUIRE(third == first);
  REQUIRE(boost::json::parse(first).as_array().size() == 3);
}

TEST_CASE("Appending continues from the leaf without dropping anything",
          "[transcript]") {
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

