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
  auto recent = ctx.Recent(1);
  REQUIRE(recent.size() == 1);
  REQUIRE(recent[0].role == "assistant");

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

TEST_CASE("Transcript::Compact with defaults trims nothing under threshold", "[workspace]") {
  Transcript t;
  for (int i = 1; i <= 20; ++i) {
    ChatMessage msg;
    msg.id = i;
    msg.role = "user";
    msg.content = "msg" + std::to_string(i);
    t.Append(msg);
  }
  REQUIRE(t.Size() == 20);
  t.Compact();  // defaults: keep_head=10, keep_tail=50; 20 <= 60 → no-op
  REQUIRE(t.Size() == 20);
}

TEST_CASE("Transcript::Compact parameterized keeps head, summary, and tail", "[workspace]") {
  Transcript t;
  for (int i = 1; i <= 20; ++i) {
    ChatMessage msg;
    msg.id = i;
    msg.role = "user";
    msg.content = "msg" + std::to_string(i);
    t.Append(msg);
  }
  t.Compact(2, 3);
  auto h = t.GetHistory();
  REQUIRE(h.size() == 6);            // 2 head + 1 summary + 3 tail
  REQUIRE(h[0].content == "msg1");
  REQUIRE(h[1].content == "msg2");
  REQUIRE(h[2].role == "system");    // summary marker
  REQUIRE(h[3].content == "msg18");
  REQUIRE(h[4].content == "msg19");
  REQUIRE(h[5].content == "msg20");
}

TEST_CASE("Transcript::Compact preserves tool-call pairing", "[workspace]") {
  Transcript t;
  for (int i = 1; i <= 10; ++i) {
    ChatMessage msg;
    msg.id = i;
    msg.role = "user";
    msg.content = "u" + std::to_string(i);
    t.Append(msg);
  }
  ChatMessage asst;
  asst.id = 11;
  asst.role = "assistant";
  asst.tool_calls_json = R"([{"id":"call_1","function":{"name":"ls","arguments":{}}}])";
  t.Append(asst);
  ChatMessage tool;
  tool.id = 12;
  tool.role = "tool";
  tool.tool_call_id = "call_1";
  t.Append(tool);

  // keep_head=6 places the assistant tool-call message inside the trimmed region,
  // so the pairing guard must pull both it and its tool response into the tail.
  t.Compact(6, 2);
  auto h = t.GetHistory();
  bool found_asst = false;
  bool found_tool = false;
  for (const auto& m : h) {
    if (m.role == "assistant" && !m.tool_calls_json.empty()) found_asst = true;
    if (m.role == "tool" && m.tool_call_id == "call_1") found_tool = true;
  }
  REQUIRE(found_asst);
  REQUIRE(found_tool);
}

TEST_CASE("Workspace::Compact forwards keep_head/keep_tail", "[workspace]") {
  Workspace ws;
  for (int i = 1; i <= 20; ++i) {
    ws.Append("user", "msg" + std::to_string(i));
  }
  ws.Compact(2, 3);
  auto h = ws.GetHistory();
  REQUIRE(h.size() == 6);
  REQUIRE(h[0].content == "msg1");
  REQUIRE(h[5].content == "msg20");
}
