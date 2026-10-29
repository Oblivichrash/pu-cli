// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/text.hpp"
#include "pu/session/request.hpp"
#include "pu/session/session.hpp"
#include <boost/json.hpp>

#include <map>
#include <string>
#include <vector>

using namespace pu;

TEST_CASE("Conversation basic operations", "[conversation]") {
  Conversation ctx;
  ctx.Append("user", "Hello");
  ctx.Append("assistant", "Hi there!");

  REQUIRE(ctx.History().size() == 2);
  auto history = ctx.History();
  REQUIRE(history.size() == 2);
  REQUIRE(history[1].role == "assistant");
}

TEST_CASE("Conversation serialization round-trips", "[conversation]") {
  Conversation ws;
  ws.Append("user", "hello");

  const boost::json::value saved = ws.Serialize();
  auto restored = Conversation::Deserialize(saved);

  REQUIRE(restored != nullptr);
  REQUIRE(restored->History().size() == 1);
}

TEST_CASE("Session serialization round-trips", "[conversation]") {
  Session session;
  session.SetAgent("chat");
  session.Messages().Append("user", "hello");

  const boost::json::value saved = session.Serialize();
  const std::string written = json::PrettyPrint(saved);
  const boost::json::value reparsed = boost::json::parse(written);

  auto restored = Session::Deserialize(reparsed);
  REQUIRE(restored != nullptr);
  REQUIRE(restored->Messages().History().size() == 1);
  REQUIRE(restored->Messages().History()[0].content == "hello");
  REQUIRE(restored->Spec().agent_name == "chat");
}

TEST_CASE("A message holding invalid UTF-8 survives a save and load", "[conversation]") {
  Session session;
  session.Messages().Append("assistant", "Request failed: \xB2\xBB\xCA\xC7");

  const std::string written = json::PrettyPrint(session.Serialize());
  REQUIRE(text::IsValidUtf8(written));

  auto restored = Session::Deserialize(boost::json::parse(written));
  REQUIRE(restored != nullptr);
  REQUIRE(restored->Messages().History().size() == 1);
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
  auto h = restored->History();
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
  auto h = t.History();
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
  asst.reasoning.push_back(ReasoningBlock{"because", {}, nullptr});
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

  auto h = t.History();
  REQUIRE(h.size() == 21);
  REQUIRE(h[0].content == "msg1");
  REQUIRE(h[19].content == "msg20");
  REQUIRE(h[20].content == "after");
}

namespace {

// A page as `execute_bash` writes one, with the one field this file is about.
ChatMessage PageMessage(const char* call_id, const char* buffer, std::size_t rank) {
  ChatMessage tool;
  tool.role = "tool";
  tool.tool_name = "execute_bash";
  tool.tool_call_id = call_id;
  tool.content = std::string(R"({"success":true,"output":"line one\n","buffer":{"id":")") + buffer +
                 R"(","source":"s3f2a1c4","rank":)" + std::to_string(rank) +
                 R"(},"page":{"first":1,"last":1},"extent":{"lines":1,"retained":1,"truncated":false}})";
  return tool;
}

}  // namespace

TEST_CASE("A page keeps where it came from through a save and a load", "[conversation]") {
  Conversation t;
  t.Append(PageMessage("call_1", "o7f3a-1", 1));
  t.Append(PageMessage("call_2", "o7f3a-2", 2));

  const boost::json::value saved = t.Serialize();
  const auto& nodes = saved.at("history").at("nodes").as_array();
  REQUIRE(nodes.size() == 2);
  // Written down rather than left to be parsed again: the file says which run answered, so a
  // reader does not have to re-derive it from text it already read once. The array is the
  // graph's storage order rather than the order the messages were appended, so each node is
  // matched to the run it names instead of to a position.
  std::map<std::string, std::size_t> ranks;
  for (const auto& node : nodes) {
    REQUIRE(node.at("origin").is_object());
    ranks[std::string(node.at("origin").at("buffer").as_string())] =
        static_cast<std::size_t>(node.at("origin").at("rank").as_uint64());
  }
  REQUIRE(ranks.size() == 2);
  REQUIRE(ranks.at("o7f3a-1") == 1);
  REQUIRE(ranks.at("o7f3a-2") == 2);

  auto restored = Conversation::Deserialize(saved);
  REQUIRE(restored != nullptr);
  const auto chain = restored->Graph().Chain();
  REQUIRE(chain.size() == 2);
  const auto* receipt = std::get_if<session::ToolPayload>(&chain[1]->payload);
  REQUIRE(receipt != nullptr);
  REQUIRE(receipt->origin.has_value());
  REQUIRE(receipt->origin->buffer == "o7f3a-2");
  REQUIRE(receipt->origin->rank == 2);

  // The point of keeping it: elision still fires on a transcript that was reloaded, without
  // the projection reading a single receipt's text to find out what is a page.
  const std::vector<ChatMessage> messages = session::BuildRequestPath(restored->Graph(), {});
  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].content.find("superseded_by") != std::string::npos);
  REQUIRE(messages[1].content.find("line one") != std::string::npos);
}

TEST_CASE("A tool result that is no page carries no origin", "[conversation]") {
  Conversation t;
  ChatMessage tool;
  tool.role = "tool";
  tool.tool_name = "write_file";
  tool.tool_call_id = "call_1";
  tool.content = R"({"success":true,"output":"","error":"","exit_code":0})";
  t.Append(tool);

  const boost::json::value saved = t.Serialize();
  const auto& node = saved.at("history").at("nodes").as_array()[0];
  // Nothing to say, so nothing said: every other tool call in a long conversation would
  // otherwise carry an origin that names no run.
  REQUIRE_FALSE(node.as_object().contains("origin"));
}
