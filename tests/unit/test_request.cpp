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

TEST_CASE("A selection keeps the head, a marker, and the tail", "[request]") {
  context::MessageGraph graph;
  for (int i = 1; i <= 20; ++i) AppendUser(graph, "msg" + std::to_string(i));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(
      graph, graph.leaf(), {}, session::KeepRecent{2, 3});

  REQUIRE(messages.size() == 6);
  REQUIRE(messages[0].content == "msg1");
  REQUIRE(messages[1].content == "msg2");
  REQUIRE(messages[2].role == "system");
  REQUIRE(messages[2].content.find("15 messages omitted") != std::string::npos);
  REQUIRE(messages[3].content == "msg18");
  REQUIRE(messages[4].content == "msg19");
  REQUIRE(messages[5].content == "msg20");

  // Storage is untouched, so the same conversation still holds every node.
  REQUIRE(graph.Size() == 20);
  REQUIRE(graph.Chain().size() == 20);
}

TEST_CASE("A selection that fits the conversation sends everything", "[request]") {
  context::MessageGraph graph;
  for (int i = 1; i <= 5; ++i) AppendUser(graph, "msg" + std::to_string(i));

  const std::vector<ChatMessage> messages = session::BuildRequestPath(
      graph, graph.leaf(), {}, session::KeepRecent{10, 50});

  REQUIRE(messages.size() == 5);
  for (const ChatMessage& msg : messages) REQUIRE(msg.role == "user");
}

TEST_CASE("A selection keeps a tool call that has no receipt yet", "[request]") {
  context::MessageGraph graph;
  for (int i = 1; i <= 9; ++i) AppendUser(graph, "u" + std::to_string(i));

  context::AssistantPayload assistant;
  assistant.tool_calls.push_back(context::ToolCallRecord{
      "call_1", "ls", boost::json::parse(R"({"path":"."})")});
  graph.AppendAfterLeaf(std::move(assistant));
  AppendUser(graph, "tail one");
  AppendUser(graph, "tail two");

  // The boundary lands after the unanswered call, so without the guard the call
  // would be dropped while the conversation continues as if nothing was asked.
  const std::vector<ChatMessage> messages = session::BuildRequestPath(
      graph, graph.leaf(), {}, session::KeepRecent{6, 2});

  bool found_call = false;
  for (const ChatMessage& msg : messages) {
    if (msg.HasToolCalls() && msg.tool_calls.as_array()[0].at("id") == "call_1") {
      found_call = true;
    }
  }
  REQUIRE(found_call);
  REQUIRE(messages.back().content == "tail two");
  REQUIRE(graph.Size() == 12);
}

TEST_CASE("A receipt after the boundary travels with its call", "[request]") {
  context::MessageGraph graph;
  for (int i = 1; i <= 9; ++i) AppendUser(graph, "u" + std::to_string(i));

  context::AssistantPayload assistant;
  assistant.tool_calls.push_back(context::ToolCallRecord{
      "call_1", "ls", boost::json::parse(R"({"path":"."})")});
  graph.AppendAfterLeaf(std::move(assistant));

  context::ToolPayload receipt;
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "ls";
  receipt.content.emplace_back(context::TextPart{"file.txt"});
  graph.AppendAfterLeaf(std::move(receipt));
  AppendUser(graph, "tail");

  const std::vector<ChatMessage> messages = session::BuildRequestPath(
      graph, graph.leaf(), {}, session::KeepRecent{6, 2});

  // The guard keeps a call whose receipt never arrived; a receipt whose call
  // falls outside the boundary is not pulled back, which is the documented gap.
  bool found_receipt = false;
  for (const ChatMessage& msg : messages) {
    if (msg.role == "tool" && msg.tool_call_id == "call_1") found_receipt = true;
  }
  REQUIRE(found_receipt);
  REQUIRE(graph.Size() == 12);
}

TEST_CASE("Two selections over one conversation give different requests",
          "[request]") {
  context::MessageGraph graph;
  for (int i = 1; i <= 20; ++i) AppendUser(graph, "msg" + std::to_string(i));

  const std::vector<ChatMessage> trimmed = session::BuildRequestPath(
      graph, graph.leaf(), {}, session::KeepRecent{2, 2});
  const std::vector<ChatMessage> whole =
      session::BuildRequestPath(graph, graph.leaf(), {});

  REQUIRE(trimmed.size() == 5);
  REQUIRE(whole.size() == 20);

  // Re-rendering the same graph after the trimmed request still sees everything,
  // which is what makes a lost range recoverable.
  REQUIRE(session::BuildRequestPath(graph, graph.leaf(), {}).size() == 20);
}
