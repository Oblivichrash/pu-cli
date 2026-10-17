// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/session/graph.hpp"

#include <string>
#include <vector>

using namespace pu;

namespace {

session::MessagePayload User(const std::string& text) {
  session::UserPayload user;
  user.content = text;
  return user;
}

session::MessagePayload Assistant(const std::string& text,
                                  std::vector<session::ToolCallRecord> calls = {}) {
  session::AssistantPayload assistant;
  assistant.content = text;
  assistant.tool_calls = std::move(calls);
  return assistant;
}

session::MessagePayload Receipt(const std::string& call_id, const std::string& text = "ok") {
  session::ToolPayload receipt;
  receipt.tool_call_id = call_id;
  receipt.content = text;
  return receipt;
}

std::string TextOf(const session::MessageNode& node) {
  if (const auto* user = std::get_if<session::UserPayload>(&node.payload)) {
    return user->content;
  }
  if (const auto* assistant = std::get_if<session::AssistantPayload>(&node.payload)) {
    return assistant->content;
  }
  if (const auto* system = std::get_if<session::SystemPayload>(&node.payload)) {
    return system->content;
  }
  return std::get<session::ToolPayload>(node.payload).content;
}

}  // namespace

TEST_CASE("An appended node links to the previous leaf", "[context][graph]") {
  session::MessageGraph graph;
  REQUIRE(graph.LeafHasUnfinishedToolCalls() == false);

  graph.AppendAfterLeaf(User("first"));
  graph.AppendAfterLeaf(Assistant("second"));

  const std::vector<const session::MessageNode*> chain = graph.Chain();
  REQUIRE(chain.size() == 2);
  REQUIRE(TextOf(*chain[0]) == "first");
  REQUIRE(TextOf(*chain[1]) == "second");
  REQUIRE(chain[0]->parent.empty());
  REQUIRE(chain[1]->parent == chain[0]->id);
  REQUIRE(graph.Size() == 2);
}

TEST_CASE("A receipt completes the record it answers", "[context][graph]") {
  session::MessageGraph graph;
  session::ToolCallRecord record;
  record.id = "call_1";
  record.name = "ls";
  graph.AppendAfterLeaf(Assistant("", {record}));

  REQUIRE(graph.LeafHasUnfinishedToolCalls());

  graph.AppendAfterLeaf(Receipt("call_1"));

  const std::vector<const session::MessageNode*> chain = graph.Chain();
  const auto& stored = std::get<session::AssistantPayload>(chain[0]->payload).tool_calls.at(0);
  REQUIRE(stored.status == session::ToolCallStatus::kCompleted);
}

TEST_CASE("A receipt for another call leaves the record pending", "[context][graph]") {
  session::MessageGraph graph;
  session::ToolCallRecord record;
  record.id = "call_1";
  graph.AppendAfterLeaf(Assistant("", {record}));
  graph.AppendAfterLeaf(Receipt("call_2"));

  const std::vector<const session::MessageNode*> chain = graph.Chain();
  const auto& stored = std::get<session::AssistantPayload>(chain[0]->payload).tool_calls.at(0);
  REQUIRE(stored.status == session::ToolCallStatus::kPending);
}

TEST_CASE("A receipt completes a record that sits further back", "[context][graph]") {
  session::MessageGraph graph;
  session::ToolCallRecord first;
  first.id = "call_1";
  session::ToolCallRecord second;
  second.id = "call_2";
  graph.AppendAfterLeaf(Assistant("", {first, second}));
  graph.AppendAfterLeaf(Receipt("call_1"));
  graph.AppendAfterLeaf(Receipt("call_2"));

  const auto& calls = std::get<session::AssistantPayload>(graph.Chain()[0]->payload).tool_calls;
  REQUIRE(calls.at(0).status == session::ToolCallStatus::kCompleted);
  REQUIRE(calls.at(1).status == session::ToolCallStatus::kCompleted);
}
