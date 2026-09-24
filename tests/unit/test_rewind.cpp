// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pu/session/session.hpp"

using namespace pu;

TEST_CASE("A step back is abandoned only when something replaces it", "[session][rewind]") {
  Workspace ws;
  ws.Append("user", "one");
  ws.Append("assistant", "two");
  ws.Append("user", "three");

  REQUIRE(ws.HistorySize() == 3);

  REQUIRE(ws.RewindBefore(3));
  REQUIRE(ws.HistorySize() == 2);

  // Still stored: stepping back costs nothing until the next message lands.
  REQUIRE(ws.GetGraph().Size() == 3);

  // The append replaces the turns after the new leaf, so the store ends up
  // holding exactly the conversation the view shows.
  ws.Append("user", "three again");
  REQUIRE(ws.HistorySize() == 3);
  REQUIRE(ws.GetHistory()[2].content == "three again");
  REQUIRE(ws.GetGraph().Size() == 3);
  REQUIRE(ws.GetGraph().Size() == ws.HistorySize());
}

TEST_CASE("Rewinding before the first turn empties the view, not the store", "[session][rewind]") {
  Workspace ws;
  ws.Append("user", "one");
  ws.Append("assistant", "two");

  REQUIRE(ws.RewindBefore(1));
  REQUIRE(ws.HistorySize() == 0);
  REQUIRE(ws.GetGraph().Size() == 2);

  ws.Append("user", "restarted");
  REQUIRE(ws.HistorySize() == 1);
  REQUIRE(ws.GetHistory()[0].content == "restarted");
  REQUIRE(ws.GetGraph().Size() == 1);
}

TEST_CASE("Rewinding refuses a position that is not there", "[session][rewind]") {
  Workspace ws;
  ws.Append("user", "one");

  REQUIRE_FALSE(ws.RewindBefore(0));
  REQUIRE_FALSE(ws.RewindBefore(2));
  REQUIRE(ws.HistorySize() == 1);
}

TEST_CASE("Rewinding is refused while a tool call is pending", "[session][rewind]") {
  Session session;
  session.GetWorkspace().Append("user", "one");

  ChatMessage assistant;
  assistant.role = context::kAssistantRole;
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"read_file","arguments":{}}}])");
  session.GetWorkspace().Append(assistant);

  REQUIRE(session.GetWorkspace().HasPendingToolCalls());
  REQUIRE_THROWS_AS(session.GetWorkspace().RewindBefore(1), std::exception);
}

TEST_CASE("A replaced turn leaves nothing behind across a save and a load", "[session][rewind]") {
  Session session;
  session.GetWorkspace().Append("user", "one");
  session.GetWorkspace().Append("assistant", "two");
  REQUIRE(session.GetWorkspace().RewindBefore(2));
  session.GetWorkspace().Append("user", "two again");

  auto restored = Session::Deserialize(session.Serialize());
  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetWorkspace().HistorySize() == 2);
  // The file carries the replacement and not the turn it replaced.
  REQUIRE(restored->GetWorkspace().GetGraph().Size() == 2);
  REQUIRE(restored->GetWorkspace().GetHistory()[1].content == "two again");
}

TEST_CASE("A replaced turn stores what sending the new text from the start would",
          "[session][rewind]") {
  Workspace edited;
  edited.Append("user", "one");
  edited.Append("assistant", "two");
  REQUIRE(edited.RewindBefore(2));
  edited.Append("user", "three");

  Workspace fresh;
  fresh.Append("user", "one");
  fresh.Append("user", "three");

  REQUIRE(edited.GetGraph().Size() == fresh.GetGraph().Size());
  REQUIRE(edited.HistorySize() == fresh.HistorySize());
  for (size_t i = 0; i < fresh.HistorySize(); ++i) {
    REQUIRE(edited.GetHistory()[i].role == fresh.GetHistory()[i].role);
    REQUIRE(edited.GetHistory()[i].content == fresh.GetHistory()[i].content);
  }
}
