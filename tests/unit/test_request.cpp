// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/session/request.hpp"

#include <boost/json.hpp>

#include <string>
#include <vector>

using namespace pu;

namespace {

context::MessageId AppendUser(context::MessageGraph& graph, const std::string& text) {
  context::UserPayload user;
  user.content.emplace_back(context::TextPart{text});
  return graph.AppendAfterLeaf(std::move(user)).id;
}

context::MessagePayload Assistant(const std::string& text) {
  context::AssistantPayload assistant;
  assistant.content.emplace_back(context::TextPart{text});
  return assistant;
}

}  // namespace

TEST_CASE("The system inputs come before the stored turns", "[request]") {
  context::MessageGraph graph;
  AppendUser(graph, "hello");
  graph.AppendAfterLeaf(Assistant("hi"));

  session::RequestInputs inputs;
  inputs.system_prompt = "be brief";
  inputs.environment = "=== Environment ===";

  const std::vector<ChatMessage> messages =
      session::BuildRequestPath(graph, graph.leaf(), inputs);

  REQUIRE(messages.size() == 3);
  REQUIRE(messages[0].role == "system");
  REQUIRE(messages[0].content == "be brief\n\n=== Environment ===");
  REQUIRE(messages[1].role == "user");
  REQUIRE(messages[1].content == "hello");
  REQUIRE(messages[2].role == "assistant");
  REQUIRE(messages[2].content == "hi");
}

TEST_CASE("Each system input is optional", "[request]") {
  context::MessageGraph graph;
  AppendUser(graph, "hello");

  session::RequestInputs prompt_only;
  prompt_only.system_prompt = "be brief";
  auto messages = session::BuildRequestPath(graph, graph.leaf(), prompt_only);
  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].content == "be brief");

  session::RequestInputs environment_only;
  environment_only.environment = "=== Environment ===";
  messages = session::BuildRequestPath(graph, graph.leaf(), environment_only);
  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].content == "=== Environment ===");

  messages = session::BuildRequestPath(graph, graph.leaf(), session::RequestInputs{});
  REQUIRE(messages.size() == 1);
  REQUIRE(messages[0].role == "user");
}

TEST_CASE("An empty conversation still carries its system inputs", "[request]") {
  const context::MessageGraph graph;

  session::RequestInputs inputs;
  inputs.system_prompt = "be brief";

  const std::vector<ChatMessage> messages =
      session::BuildRequestPath(graph, graph.leaf(), inputs);

  REQUIRE(messages.size() == 1);
  REQUIRE(messages[0].role == "system");
  REQUIRE(messages[0].content == "be brief");
}

TEST_CASE("The view is the path to the leaf it is asked for", "[request]") {
  context::MessageGraph graph;
  const context::MessageId first = AppendUser(graph, "one");
  const context::MessageId second = graph.AppendAfterLeaf(Assistant("two")).id;
  AppendUser(graph, "three");

  const std::vector<ChatMessage> whole =
      session::BuildRequestPath(graph, graph.leaf(), {});
  REQUIRE(whole.size() == 3);
  REQUIRE(whole[2].content == "three");

  // Rendering from an earlier node gives the prefix that ends there, which is
  // what makes a stored node a viable position to resume from.
  const std::vector<ChatMessage> from_second =
      session::BuildRequestPath(graph, second, {});
  REQUIRE(from_second.size() == 2);
  REQUIRE(from_second[0].content == "one");
  REQUIRE(from_second[1].content == "two");

  const std::vector<ChatMessage> from_first =
      session::BuildRequestPath(graph, first, {});
  REQUIRE(from_first.size() == 1);
  REQUIRE(from_first[0].content == "one");
}

TEST_CASE("A stored system node is passed through", "[request]") {
  context::MessageGraph graph;
  AppendUser(graph, "one");

  context::SystemPayload summary;
  summary.content.emplace_back(context::TextPart{"[Compressed: 4 messages omitted]"});
  summary.is_synthetic = true;
  graph.AppendAfterLeaf(std::move(summary));

  session::RequestInputs inputs;
  inputs.system_prompt = "be brief";

  const std::vector<ChatMessage> messages =
      session::BuildRequestPath(graph, graph.leaf(), inputs);

  REQUIRE(messages.size() == 3);
  REQUIRE(messages[0].content == "be brief");
  REQUIRE(messages[1].content == "one");
  REQUIRE(messages[2].content == "[Compressed: 4 messages omitted]");
}

TEST_CASE("Tool calls and receipts survive the request view", "[request]") {
  context::MessageGraph graph;
  context::AssistantPayload assistant;
  assistant.content.emplace_back(context::TextPart{"checking"});
  assistant.reasoning = context::Reasoning{"openai", "sig", "because"};
  assistant.tool_calls.push_back(context::ToolCallRecord{
      "call_1", "ls", boost::json::parse(R"({"path":"."})")});
  graph.AppendAfterLeaf(std::move(assistant));

  context::ToolPayload receipt;
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "ls";
  receipt.content.emplace_back(context::TextPart{"file.txt"});
  graph.AppendAfterLeaf(std::move(receipt));

  const std::vector<ChatMessage> messages =
      session::BuildRequestPath(graph, graph.leaf(), {});

  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].HasToolCalls());
  REQUIRE(messages[0].reasoning_content == "because");
  REQUIRE(messages[0].tool_calls.as_array()[0].at("function").at("arguments")
              .at("path") == ".");
  REQUIRE(messages[1].role == "tool");
  REQUIRE(messages[1].tool_name == "ls");
  REQUIRE(messages[1].tool_call_id == "call_1");
}

TEST_CASE("Rendered positions follow the view, not the stored ids", "[request]") {
  context::MessageGraph graph;
  AppendUser(graph, "one");
  const context::MessageId second = graph.AppendAfterLeaf(Assistant("two")).id;
  AppendUser(graph, "three");

  const std::vector<ChatMessage> messages =
      session::BuildRequestPath(graph, graph.leaf(), {});

  REQUIRE(messages[0].id == 1);
  REQUIRE(messages[1].id == 2);
  REQUIRE(messages[2].id == 3);

  const std::vector<ChatMessage> prefix = session::BuildRequestPath(graph, second, {});
  REQUIRE(prefix[0].id == 1);
  REQUIRE(prefix[1].id == 2);
}
