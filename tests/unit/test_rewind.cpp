// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "pu/session/session.hpp"

using namespace pu;

TEST_CASE("A step back is abandoned only when something replaces it", "[session][rewind]") {
  Conversation ws;
  ws.Append("user", "one");
  ws.Append("assistant", "two");
  ws.Append("user", "three");

  REQUIRE(ws.History().size() == 3);

  REQUIRE(ws.RewindBefore(3));
  REQUIRE(ws.History().size() == 2);

  REQUIRE(ws.Graph().Size() == 3);

  ws.Append("user", "three again");
  REQUIRE(ws.History().size() == 3);
  REQUIRE(ws.History()[2].content == "three again");
  REQUIRE(ws.Graph().Size() == 3);
  REQUIRE(ws.Graph().Size() == ws.History().size());
}

TEST_CASE("Rewinding before the first turn empties the view, not the store", "[session][rewind]") {
  Conversation ws;
  ws.Append("user", "one");
  ws.Append("assistant", "two");

  REQUIRE(ws.RewindBefore(1));
  REQUIRE(ws.History().size() == 0);
  REQUIRE(ws.Graph().Size() == 2);

  ws.Append("user", "restarted");
  REQUIRE(ws.History().size() == 1);
  REQUIRE(ws.History()[0].content == "restarted");
  REQUIRE(ws.Graph().Size() == 1);
}

TEST_CASE("Rewinding refuses a position that is not there", "[session][rewind]") {
  Conversation ws;
  ws.Append("user", "one");

  REQUIRE_FALSE(ws.RewindBefore(0));
  REQUIRE_FALSE(ws.RewindBefore(2));
  REQUIRE(ws.History().size() == 1);
}

TEST_CASE("Rewinding is refused while a tool call is pending", "[session][rewind]") {
  Session session;
  session.Messages().Append("user", "one");

  ChatMessage assistant;
  assistant.role = session::kAssistantRole;
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}}])");
  session.Messages().Append(assistant);

  REQUIRE(session.Messages().HasPendingToolCalls());
  REQUIRE_THROWS_AS(session.Messages().RewindBefore(1), RequestRefused);
  REQUIRE_THROWS_AS(session.Messages().RewindBefore(1), std::exception);
}

TEST_CASE("Sealing an interrupted turn lets a rewind through again", "[session][rewind]") {
  Session session;
  session.Messages().Append("user", "one");

  ChatMessage assistant;
  assistant.role = session::kAssistantRole;
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}},)"
      R"({"id":"call_2","type":"function","function":{"name":"write_file","arguments":{}}}])");
  session.Messages().Append(assistant);

  REQUIRE(session.Messages().HasPendingToolCalls());

  session.Messages().SealInterruptedToolCalls();

  REQUIRE_FALSE(session.Messages().HasPendingToolCalls());
  REQUIRE(session.Messages().RewindBefore(1));
}

TEST_CASE("Sealing an interrupted turn hands the model a receipt for every call it never ran",
          "[session][rewind]") {
  Session session;
  session.Messages().Append("user", "one");

  ChatMessage assistant;
  assistant.role = session::kAssistantRole;
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}},)"
      R"({"id":"call_2","type":"function","function":{"name":"write_file","arguments":{}}}])");
  session.Messages().Append(assistant);

  session.Messages().SealInterruptedToolCalls();

  const std::vector<ChatMessage> history = session.Messages().History();
  REQUIRE(history.size() == 4);

  size_t receipts = 0;
  for (const ChatMessage& message : history) {
    if (message.role == session::kToolRole) {
      REQUIRE(message.content == session::kCancelledReceipt);
      ++receipts;
    }
  }
  REQUIRE(receipts == 2);
  REQUIRE(history[1].tool_calls.as_array().size() == 2);
  REQUIRE(history[2].role == session::kToolRole);
  REQUIRE(history[3].role == session::kToolRole);
}

TEST_CASE("An interrupted turn survives a save and a load as interrupted",
          "[session][rewind][schema]") {
  Session session;
  session.Messages().Append("user", "one");

  ChatMessage assistant;
  assistant.role = session::kAssistantRole;
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}}])");
  session.Messages().Append(assistant);
  session.Messages().SealInterruptedToolCalls();

  const boost::json::value saved = session.Serialize();
  std::unique_ptr<Session> loaded = Session::Deserialize(saved);
  REQUIRE(loaded != nullptr);

  REQUIRE_FALSE(loaded->Messages().HasPendingToolCalls());
  REQUIRE(loaded->Messages().RewindBefore(1));
}

TEST_CASE("An untouched pending call still reads as pending after a save and a load",
          "[session][rewind][schema]") {
  Session session;
  session.Messages().Append("user", "one");

  ChatMessage assistant;
  assistant.role = session::kAssistantRole;
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}}])");
  session.Messages().Append(assistant);

  const boost::json::value saved = session.Serialize();
  std::unique_ptr<Session> loaded = Session::Deserialize(saved);
  REQUIRE(loaded != nullptr);

  REQUIRE(loaded->Messages().HasPendingToolCalls());
}

TEST_CASE("A replaced turn leaves nothing behind across a save and a load", "[session][rewind]") {
  Session session;
  session.Messages().Append("user", "one");
  session.Messages().Append("assistant", "two");
  REQUIRE(session.Messages().RewindBefore(2));
  session.Messages().Append("user", "two again");

  auto restored = Session::Deserialize(session.Serialize());
  REQUIRE(restored != nullptr);
  REQUIRE(restored->Messages().History().size() == 2);
  REQUIRE(restored->Messages().Graph().Size() == 2);
  REQUIRE(restored->Messages().History()[1].content == "two again");
}

TEST_CASE("A replaced turn stores what sending the new text from the start would",
          "[session][rewind]") {
  Conversation edited;
  edited.Append("user", "one");
  edited.Append("assistant", "two");
  REQUIRE(edited.RewindBefore(2));
  edited.Append("user", "three");

  Conversation fresh;
  fresh.Append("user", "one");
  fresh.Append("user", "three");

  REQUIRE(edited.Graph().Size() == fresh.Graph().Size());
  REQUIRE(edited.History().size() == fresh.History().size());
  for (size_t i = 0; i < fresh.History().size(); ++i) {
    REQUIRE(edited.History()[i].role == fresh.History()[i].role);
    REQUIRE(edited.History()[i].content == fresh.History()[i].content);
  }
}
