// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pu/session/session.hpp"

using namespace pu;

TEST_CASE("A step back is abandoned only when something replaces it", "[session][rewind]") {
  Conversation ws;
  ws.Append("user", "one");
  ws.Append("assistant", "two");
  ws.Append("user", "three");

  REQUIRE(ws.GetHistory().size() == 3);

  REQUIRE(ws.RewindBefore(3));
  REQUIRE(ws.GetHistory().size() == 2);

  REQUIRE(ws.GetGraph().Size() == 3);

  ws.Append("user", "three again");
  REQUIRE(ws.GetHistory().size() == 3);
  REQUIRE(ws.GetHistory()[2].content == "three again");
  REQUIRE(ws.GetGraph().Size() == 3);
  REQUIRE(ws.GetGraph().Size() == ws.GetHistory().size());
}

TEST_CASE("Rewinding before the first turn empties the view, not the store", "[session][rewind]") {
  Conversation ws;
  ws.Append("user", "one");
  ws.Append("assistant", "two");

  REQUIRE(ws.RewindBefore(1));
  REQUIRE(ws.GetHistory().size() == 0);
  REQUIRE(ws.GetGraph().Size() == 2);

  ws.Append("user", "restarted");
  REQUIRE(ws.GetHistory().size() == 1);
  REQUIRE(ws.GetHistory()[0].content == "restarted");
  REQUIRE(ws.GetGraph().Size() == 1);
}

TEST_CASE("Rewinding refuses a position that is not there", "[session][rewind]") {
  Conversation ws;
  ws.Append("user", "one");

  REQUIRE_FALSE(ws.RewindBefore(0));
  REQUIRE_FALSE(ws.RewindBefore(2));
  REQUIRE(ws.GetHistory().size() == 1);
}

TEST_CASE("Rewinding is refused while a tool call is pending", "[session][rewind]") {
  Session session;
  session.GetConversation().Append("user", "one");

  ChatMessage assistant;
  assistant.role = context::kAssistantRole;
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}}])");
  session.GetConversation().Append(assistant);

  REQUIRE(session.GetConversation().HasPendingToolCalls());
  REQUIRE_THROWS_AS(session.GetConversation().RewindBefore(1), RequestRefused);
  REQUIRE_THROWS_AS(session.GetConversation().RewindBefore(1), std::exception);
}

TEST_CASE("A replaced turn leaves nothing behind across a save and a load", "[session][rewind]") {
  Session session;
  session.GetConversation().Append("user", "one");
  session.GetConversation().Append("assistant", "two");
  REQUIRE(session.GetConversation().RewindBefore(2));
  session.GetConversation().Append("user", "two again");

  auto restored = Session::Deserialize(session.Serialize());
  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetConversation().GetHistory().size() == 2);
  REQUIRE(restored->GetConversation().GetGraph().Size() == 2);
  REQUIRE(restored->GetConversation().GetHistory()[1].content == "two again");
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

  REQUIRE(edited.GetGraph().Size() == fresh.GetGraph().Size());
  REQUIRE(edited.GetHistory().size() == fresh.GetHistory().size());
  for (size_t i = 0; i < fresh.GetHistory().size(); ++i) {
    REQUIRE(edited.GetHistory()[i].role == fresh.GetHistory()[i].role);
    REQUIRE(edited.GetHistory()[i].content == fresh.GetHistory()[i].content);
  }
}
