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
  user.content = text;
  return graph.AppendAfterLeaf(std::move(user)).id;
}

context::MessagePayload Assistant(const std::string& text) {
  context::AssistantPayload assistant;
  assistant.content = text;
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

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);

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
  auto messages = session::BuildRequestPath(graph, prompt_only);
  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].content == "be brief");

  session::RequestInputs environment_only;
  environment_only.environment = "=== Environment ===";
  messages = session::BuildRequestPath(graph, environment_only);
  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].content == "=== Environment ===");

  messages = session::BuildRequestPath(graph, session::RequestInputs{});
  REQUIRE(messages.size() == 1);
  REQUIRE(messages[0].role == "user");
}

TEST_CASE("An empty conversation still carries its system inputs", "[request]") {
  const context::MessageGraph graph;

  session::RequestInputs inputs;
  inputs.system_prompt = "be brief";

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);

  REQUIRE(messages.size() == 1);
  REQUIRE(messages[0].role == "system");
  REQUIRE(messages[0].content == "be brief");
}

TEST_CASE("A stored system node is passed through", "[request]") {
  context::MessageGraph graph;
  AppendUser(graph, "one");

  context::SystemPayload note;
  note.content = "a note from the caller";
  graph.AppendAfterLeaf(std::move(note));

  session::RequestInputs inputs;
  inputs.system_prompt = "be brief";

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, inputs);

  REQUIRE(messages.size() == 3);
  REQUIRE(messages[0].content == "be brief");
  REQUIRE(messages[1].content == "one");
  REQUIRE(messages[2].content == "a note from the caller");
}

TEST_CASE("Tool calls and receipts survive the request view", "[request]") {
  context::MessageGraph graph;
  context::AssistantPayload assistant;
  assistant.content = "checking";
  assistant.reasoning = context::Reasoning{"because"};
  assistant.tool_calls.push_back(
      context::ToolCallRecord{"call_1", "ls", boost::json::parse(R"({"path":"."})")});
  graph.AppendAfterLeaf(std::move(assistant));

  context::ToolPayload receipt;
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "ls";
  receipt.content = "file.txt";
  graph.AppendAfterLeaf(std::move(receipt));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  REQUIRE(messages.size() == 2);
  REQUIRE(messages[0].HasToolCalls());
  REQUIRE(messages[0].reasoning_content == "because");
  REQUIRE(messages[0].tool_calls.as_array()[0].at("function").at("arguments").at("path") == ".");
  REQUIRE(messages[1].role == "tool");
  REQUIRE(messages[1].tool_name == "ls");
  REQUIRE(messages[1].tool_call_id == "call_1");
}

TEST_CASE("Rendered positions follow the view, not the stored ids", "[request]") {
  context::MessageGraph graph;
  AppendUser(graph, "one");
  graph.AppendAfterLeaf(Assistant("two"));
  AppendUser(graph, "three");

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  REQUIRE(messages[0].id == 1);
  REQUIRE(messages[1].id == 2);
  REQUIRE(messages[2].id == 3);
}

TEST_CASE("Every stored node reaches the model, with its tool receipt", "[request]") {
  context::MessageGraph graph;
  for (int i = 1; i <= 20; ++i) AppendUser(graph, "msg" + std::to_string(i));

  context::AssistantPayload assistant;
  assistant.tool_calls.push_back(
      context::ToolCallRecord{"call_1", "ls", boost::json::parse(R"({"path":"."})")});
  graph.AppendAfterLeaf(std::move(assistant));

  context::ToolPayload receipt;
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "ls";
  receipt.content = "file.txt";
  graph.AppendAfterLeaf(std::move(receipt));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(graph, {});

  REQUIRE(messages.size() == graph.Size());
  REQUIRE(messages.size() == 22);

  // The receipt follows the call it answers, which is the shape a provider
  // requires and the reason the request is not shortened.
  bool call_followed_by_receipt = false;
  for (std::size_t i = 0; i + 1 < messages.size(); ++i) {
    if (messages[i].HasToolCalls() && messages[i + 1].role == "tool" &&
        messages[i + 1].tool_call_id == "call_1") {
      call_followed_by_receipt = true;
    }
  }
  REQUIRE(call_followed_by_receipt);
}
