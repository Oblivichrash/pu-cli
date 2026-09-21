// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/context/graph.hpp"

#include <string>
#include <vector>

using namespace pu;

namespace {

context::MessagePayload User(const std::string& text) {
  context::UserPayload user;
  user.content.emplace_back(context::TextPart{text});
  return user;
}

context::MessagePayload Assistant(const std::string& text,
                                  std::vector<context::ToolCallRecord> calls = {}) {
  context::AssistantPayload assistant;
  assistant.content.emplace_back(context::TextPart{text});
  assistant.tool_calls = std::move(calls);
  return assistant;
}

context::MessagePayload Receipt(const std::string& call_id,
                                const std::string& text = "ok") {
  context::ToolPayload receipt;
  receipt.tool_call_id = call_id;
  receipt.content.emplace_back(context::TextPart{text});
  return receipt;
}

std::string TextOf(const context::MessageNode& node) {
  if (const auto* user = std::get_if<context::UserPayload>(&node.payload)) {
    return context::FlattenText(user->content);
  }
  if (const auto* assistant = std::get_if<context::AssistantPayload>(&node.payload)) {
    return context::FlattenText(assistant->content);
  }
  if (const auto* system = std::get_if<context::SystemPayload>(&node.payload)) {
    return context::FlattenText(system->content);
  }
  return context::FlattenText(std::get<context::ToolPayload>(node.payload).content);
}

}  // namespace

TEST_CASE("An appended node links to the previous leaf", "[context][graph]") {
  context::MessageGraph graph;
  REQUIRE(graph.empty());
  REQUIRE(graph.LeafHasUnfinishedToolCalls() == false);

  graph.AppendAfterLeaf(User("first"));
  graph.AppendAfterLeaf(Assistant("second"));

  const std::vector<const context::MessageNode*> chain = graph.Chain();
  REQUIRE(chain.size() == 2);
  REQUIRE(TextOf(*chain[0]) == "first");
  REQUIRE(TextOf(*chain[1]) == "second");
  REQUIRE(chain[0]->parents.empty());
  REQUIRE(chain[1]->parents == std::vector<context::MessageId>{chain[0]->id});
  REQUIRE(graph.Size() == 2);
}

TEST_CASE("Order comes from parent links, not from insertion", "[context][graph]") {
  context::MessageGraph graph;
  const context::MessageNode& first = graph.AppendAfterLeaf(User("a"));
  const context::MessageId first_id = first.id;
  graph.AppendAfterLeaf(User("b"));

  // Detach the second node and point the leaf back at the first, so the node
  // that was inserted last is no longer on the chain.
  const std::vector<const context::MessageNode*> before = graph.Chain();
  REQUIRE(before.size() == 2);

  graph.SetLeaf(first_id);
  const std::vector<const context::MessageNode*> after = graph.Chain();
  REQUIRE(after.size() == 1);
  REQUIRE(TextOf(*after[0]) == "a");

  // The detached node is still stored, which is what makes rewind possible.
  REQUIRE(graph.Size() == 2);
}

TEST_CASE("A receipt completes the record it answers", "[context][graph]") {
  context::MessageGraph graph;
  context::ToolCallRecord record;
  record.id = "call_1";
  record.name = "ls";
  graph.AppendAfterLeaf(Assistant("", {record}));

  REQUIRE(graph.LeafHasUnfinishedToolCalls());

  graph.AppendAfterLeaf(Receipt("call_1"));

  const std::vector<const context::MessageNode*> chain = graph.Chain();
  const auto& stored =
      std::get<context::AssistantPayload>(chain[0]->payload).tool_calls.at(0);
  REQUIRE(stored.status == context::ToolCallStatus::kCompleted);
}

TEST_CASE("A receipt for another call leaves the record pending", "[context][graph]") {
  context::MessageGraph graph;
  context::ToolCallRecord record;
  record.id = "call_1";
  graph.AppendAfterLeaf(Assistant("", {record}));
  graph.AppendAfterLeaf(Receipt("call_2"));

  const std::vector<const context::MessageNode*> chain = graph.Chain();
  const auto& stored =
      std::get<context::AssistantPayload>(chain[0]->payload).tool_calls.at(0);
  REQUIRE(stored.status == context::ToolCallStatus::kPending);
}

TEST_CASE("A receipt completes a record that sits further back", "[context][graph]") {
  context::MessageGraph graph;
  context::ToolCallRecord first;
  first.id = "call_1";
  context::ToolCallRecord second;
  second.id = "call_2";
  graph.AppendAfterLeaf(Assistant("", {first, second}));
  graph.AppendAfterLeaf(Receipt("call_1"));
  graph.AppendAfterLeaf(Receipt("call_2"));

  const auto& calls =
      std::get<context::AssistantPayload>(graph.Chain()[0]->payload).tool_calls;
  REQUIRE(calls.at(0).status == context::ToolCallStatus::kCompleted);
  REQUIRE(calls.at(1).status == context::ToolCallStatus::kCompleted);
}

TEST_CASE("RetainOnly keeps the listed nodes and drops the rest", "[context][graph]") {
  context::MessageGraph graph;
  const context::MessageId first = graph.AppendAfterLeaf(User("a")).id;
  const context::MessageId second = graph.AppendAfterLeaf(User("b")).id;
  graph.AppendAfterLeaf(User("c"));

  graph.RetainOnly({first, second});
  graph.SetLeaf(second);

  REQUIRE(graph.Size() == 2);
  REQUIRE(graph.Chain().size() == 2);
  REQUIRE(TextOf(*graph.Chain()[1]) == "b");
  REQUIRE(graph.Find(second) != nullptr);
}

TEST_CASE("Add inserts without touching the leaf", "[context][graph]") {
  context::MessageGraph graph;
  const context::MessageId first = graph.AppendAfterLeaf(User("a")).id;

  context::SystemPayload summary;
  summary.content.emplace_back(context::TextPart{"[Compressed: 3 messages omitted]"});
  summary.is_synthetic = true;
  context::MessageNode node = context::MakeNode(std::move(summary));
  const context::MessageId summary_id = node.id;
  graph.Add(std::move(node));

  REQUIRE(graph.leaf() == first);
  REQUIRE(graph.Chain().size() == 1);

  graph.SetParents(first, {summary_id});
  REQUIRE(graph.Chain().size() == 2);
  REQUIRE(std::get<context::SystemPayload>(graph.Chain()[0]->payload).is_synthetic);
}
