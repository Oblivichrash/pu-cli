// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/context/message.hpp"
#include "pu/core/text.hpp"
#include "pu/core/uuid.hpp"

#include <set>
#include <string>

using namespace pu;

TEST_CASE("NewMessageId produces a lowercase UUID v4", "[context][message]") {
  const context::MessageId id = context::NewMessageId();

  REQUIRE(id.size() == 36);
  REQUIRE(id[8] == '-');
  REQUIRE(id[13] == '-');
  REQUIRE(id[18] == '-');
  REQUIRE(id[23] == '-');
  REQUIRE(id[14] == '4');
  REQUIRE(std::string("89ab").find(id[19]) != std::string::npos);
  REQUIRE(text::IsValidUtf8(id));
}

TEST_CASE("Message ids do not repeat", "[context][message]") {
  std::set<context::MessageId> ids;
  for (int i = 0; i < 500; ++i) ids.insert(context::NewMessageId());
  REQUIRE(ids.size() == 500);
}

TEST_CASE("MakeNode assigns an id and keeps the payload", "[context][message]") {
  context::UserPayload user;
  user.content.emplace_back(context::TextPart{"hello"});

  const context::MessageNode node = context::MakeNode(std::move(user));

  REQUIRE_FALSE(node.id.empty());
  REQUIRE(node.parents.empty());
  REQUIRE(std::holds_alternative<context::UserPayload>(node.payload));
  REQUIRE(context::FlattenText(std::get<context::UserPayload>(node.payload).content) ==
          "hello");
}

TEST_CASE("MakeNode records parents in order", "[context][message]") {
  const context::MessageId root = context::NewMessageId();
  const context::MessageId branch = context::NewMessageId();

  context::SystemPayload system;
  system.content.emplace_back(context::TextPart{"system"});
  const context::MessageNode node = context::MakeNode(std::move(system), {root, branch});

  REQUIRE(node.parents == std::vector<context::MessageId>{root, branch});
}

TEST_CASE("FlattenText concatenates parts in order", "[context][message]") {
  const std::vector<context::ContentPart> parts = {
      context::TextPart{"first "},
      context::TextPart{"second"},
  };

  REQUIRE(context::FlattenText(parts) == "first second");
  REQUIRE(context::FlattenText({}).empty());
}

TEST_CASE("Each role payload is distinguishable by type", "[context][message]") {
  const context::MessageNode user = context::MakeNode(context::UserPayload{});
  const context::MessageNode assistant = context::MakeNode(context::AssistantPayload{});
  const context::MessageNode system = context::MakeNode(context::SystemPayload{});
  const context::MessageNode tool = context::MakeNode(context::ToolPayload{});

  REQUIRE(std::holds_alternative<context::UserPayload>(user.payload));
  REQUIRE(std::holds_alternative<context::AssistantPayload>(assistant.payload));
  REQUIRE(std::holds_alternative<context::SystemPayload>(system.payload));
  REQUIRE(std::holds_alternative<context::ToolPayload>(tool.payload));
}

TEST_CASE("A tool call starts pending and a completed one no longer blocks",
          "[context][message]") {
  context::AssistantPayload assistant;
  assistant.content.emplace_back(context::TextPart{"let me check"});
  assistant.reasoning = context::Reasoning{"openai", "sig", R"({"raw":true})"};
  assistant.tool_calls.push_back(
      context::ToolCallRecord{"call_1", "read_file", boost::json::object{}});

  const context::MessageNode node = context::MakeNode(std::move(assistant));

  REQUIRE(context::HasUnfinishedToolCalls(node));
  REQUIRE(std::get<context::AssistantPayload>(node.payload).reasoning->signature == "sig");

  context::AssistantPayload done;
  done.tool_calls.push_back(context::ToolCallRecord{
      "call_1", "read_file", boost::json::object{}, context::ToolCallStatus::kCompleted});
  const context::MessageNode finished = context::MakeNode(std::move(done));

  REQUIRE_FALSE(context::HasUnfinishedToolCalls(finished));
}

TEST_CASE("Tool call arguments keep their JSON shape", "[context][message]") {
  context::AssistantPayload assistant;
  assistant.tool_calls.push_back(context::ToolCallRecord{
      "call_1", "read_file", boost::json::parse(R"({"path":"."})")});

  const context::MessageNode node = context::MakeNode(std::move(assistant));

  const auto& record =
      std::get<context::AssistantPayload>(node.payload).tool_calls.at(0);
  REQUIRE(record.arguments.is_object());
  REQUIRE(record.arguments.at("path") == ".");
}

TEST_CASE("Only a tool payload responds to a tool call", "[context][message]") {
  context::ToolPayload receipt;
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "read_file";
  receipt.content.emplace_back(context::TextPart{"file contents"});
  const context::MessageNode node = context::MakeNode(std::move(receipt));

  const context::ToolPayload& payload = std::get<context::ToolPayload>(node.payload);
  REQUIRE(payload.tool_call_id == "call_1");
  REQUIRE(payload.tool_name == "read_file");
  REQUIRE_FALSE(context::HasUnfinishedToolCalls(node));
}

TEST_CASE("A node carries a timestamp and parent links", "[context][message]") {
  context::MessageNode node = context::MakeNode(context::UserPayload{});
  REQUIRE(node.timestamp.empty());

  node.timestamp = "2026-09-21T10:00:00Z";
  REQUIRE(node.timestamp == "2026-09-21T10:00:00Z");
  REQUIRE(node.parents.empty());
}

TEST_CASE("The shared UUID generator meets the v4 contract", "[context][message]") {
  const std::string id = uuid::Generate();
  REQUIRE(id.size() == 36);
  REQUIRE(id[14] == '4');
  REQUIRE(std::string("89ab").find(id[19]) != std::string::npos);
  REQUIRE(uuid::Generate() != id);
}
