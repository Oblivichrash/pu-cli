// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pu/session/session.hpp"

using namespace pu;

TEST_CASE("Rewinding keeps the branch it left behind", "[session][rewind]") {
  Workspace ws;
  ws.Append("user", "one");
  ws.Append("assistant", "two");
  ws.Append("user", "three");

  REQUIRE(ws.HistorySize() == 3);

  REQUIRE(ws.RewindBefore(3));
  REQUIRE(ws.HistorySize() == 2);

  // Nothing was removed: the store still holds every node.
  REQUIRE(ws.GetGraph().Size() == 3);

  // The next append starts a new branch from turn 2.
  ws.Append("user", "three again");
  REQUIRE(ws.HistorySize() == 3);
  REQUIRE(ws.GetHistory()[2].content == "three again");
  REQUIRE(ws.GetGraph().Size() == 4);
}

TEST_CASE("Rewinding before the first turn empties the view, not the store",
          "[session][rewind]") {
  Workspace ws;
  ws.Append("user", "one");
  ws.Append("assistant", "two");

  REQUIRE(ws.RewindBefore(1));
  REQUIRE(ws.HistorySize() == 0);
  REQUIRE(ws.GetGraph().Size() == 2);

  ws.Append("user", "restarted");
  REQUIRE(ws.HistorySize() == 1);
  REQUIRE(ws.GetHistory()[0].content == "restarted");
  REQUIRE(ws.GetGraph().Size() == 3);
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

TEST_CASE("A rewound branch survives a save and a load", "[session][rewind]") {
  Session session;
  session.GetWorkspace().Append("user", "one");
  session.GetWorkspace().Append("assistant", "two");
  REQUIRE(session.GetWorkspace().RewindBefore(2));
  session.GetWorkspace().Append("user", "two again");

  auto restored = Session::Deserialize(session.Serialize());
  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetWorkspace().HistorySize() == 2);
  // The abandoned branch is still stored, so the view is shorter than the store.
  REQUIRE(restored->GetWorkspace().GetGraph().Size() == 3);
  REQUIRE(restored->GetWorkspace().GetHistory()[1].content == "two again");
}
