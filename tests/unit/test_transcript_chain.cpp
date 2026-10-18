// SPDX-License-Identifier: GPL-3.0-only

//
// The transcript cases that need nothing but memory: a turn that ran a tool is three stored
// nodes, and every way of reading that store back has to fold it into one turn. These are
// the cases the page's refresh leans on, so they run in the suite rather than beside it.
//
// The case that runs a real turn through the runtime against a stdio MCP server lives in
// tests/pending/ with the stdio cases, until the subprocess path has a server it controls.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "pu/session/message.hpp"
#include "pu/session/session.hpp"

namespace {

constexpr const char* kCallId = "call_1";

void AppendAssistantAskingFor(pu::Conversation& conversation, const std::string& call_id) {
  pu::ChatMessage assistant;
  assistant.role = pu::session::kAssistantRole;
  assistant.tool_calls = boost::json::array{boost::json::object{
      {"id", call_id},
      {"type", "function"},
      {"function", boost::json::object{{"name", "execute_bash"},
                                       {"arguments", boost::json::object{{"command", "ls"}}}}}}};
  conversation.Append(assistant);
}

void AppendStoredToolResult(pu::Conversation& conversation, const std::string& call_id) {
  pu::ChatMessage receipt;
  receipt.role = pu::session::kToolRole;
  receipt.tool_call_id = call_id;
  receipt.tool_name = "execute_bash";
  receipt.content = boost::json::serialize(boost::json::object{{"success", true},
                                                              {"blocked", false},
                                                              {"output", "a.txt\n"},
                                                              {"error", ""},
                                                              {"exit_code", 0}});
  conversation.Append(receipt);
}

// One question, one call, and the receipt that answers it: what a turn that ran a tool looks
// like once the store has it.
pu::Conversation TurnWithOneCall() {
  pu::Conversation conversation;
  conversation.Append(pu::session::kUserRole, "list the files");
  AppendAssistantAskingFor(conversation, kCallId);
  AppendStoredToolResult(conversation, kCallId);
  return conversation;
}

}  // namespace

TEST_CASE("A turn that ran a tool is three stored nodes, and the page folds it into one",
          "[transcript]") {
  const pu::Conversation conversation = TurnWithOneCall();

  // The receipt stays in the store, because the model reads it back on the next turn. The
  // page folds it into the answer that made the call, so the turn is drawn once.
  const std::vector<pu::ChatMessage> history = conversation.History();
  REQUIRE(history.size() == 3);
  REQUIRE(history[0].content == "list the files");
  REQUIRE(history[1].HasToolCalls());
  REQUIRE(history[2].role == pu::session::kToolRole);
  REQUIRE(history[2].tool_call_id == kCallId);
}

TEST_CASE("A receipt whose call left the chain is not a message of its own", "[transcript]") {
  pu::Conversation conversation;
  conversation.Append(pu::session::kUserRole, "list the files");
  AppendStoredToolResult(conversation, "call_that_left");

  // Nothing asked for it, so nothing can answer it. A page that drew it would put a result
  // beside a turn that never made the call, which is a second copy on every refresh.
  const std::vector<pu::ChatMessage> history = conversation.History();
  REQUIRE(history.size() == 1);
  REQUIRE(history[0].role == pu::session::kUserRole);
}

TEST_CASE("A turn survives a save and a load without gaining a copy", "[transcript]") {
  auto restored = pu::Conversation::Deserialize(TurnWithOneCall().Serialize());
  REQUIRE(restored != nullptr);
  REQUIRE(restored->History().size() == 3);

  restored->Append(pu::session::kUserRole, "and again");
  REQUIRE(restored->History().size() == 4);
}

TEST_CASE("A save file that names one node twice is refused", "[transcript]") {
  // An id names one node. A file naming it twice would draw the same turn twice on the next
  // load, so the load refuses it rather than keeping whichever copy came last.
  const boost::json::value saved = TurnWithOneCall().Serialize();
  const boost::json::value& nodes = saved.at("history").at("nodes");

  boost::json::array doubled;
  for (const boost::json::value& node : nodes.as_array()) doubled.push_back(node);
  doubled.push_back(doubled.front());

  boost::json::object broken_history = saved.at("history").as_object();
  broken_history["nodes"] = std::move(doubled);
  boost::json::object broken = saved.as_object();
  broken["history"] = std::move(broken_history);

  REQUIRE(pu::Conversation::Deserialize(broken) == nullptr);
}
