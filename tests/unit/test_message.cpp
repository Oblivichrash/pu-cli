// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/session/message.hpp"
#include "pu/core/text.hpp"
#include "pu/core/base.hpp"

#include <set>
#include <string>

using namespace pu;

TEST_CASE("NewMessageId produces a lowercase UUID v4", "[context][message]") {
  const session::MessageId id = session::NewMessageId();

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
  std::set<session::MessageId> ids;
  for (int i = 0; i < 500; ++i) ids.insert(session::NewMessageId());
  REQUIRE(ids.size() == 500);
}

TEST_CASE("MakeNode assigns an id and keeps the payload", "[context][message]") {
  session::UserPayload user;
  user.content = "hello";

  const session::MessageNode node = session::MakeNode(std::move(user));

  REQUIRE_FALSE(node.id.empty());
  REQUIRE(node.parent.empty());
  REQUIRE(std::holds_alternative<session::UserPayload>(node.payload));
  REQUIRE(std::get<session::UserPayload>(node.payload).content == "hello");
}

TEST_CASE("Each role payload is distinguishable by type", "[context][message]") {
  const session::MessageNode user = session::MakeNode(session::UserPayload{});
  const session::MessageNode assistant = session::MakeNode(session::AssistantPayload{});
  const session::MessageNode system = session::MakeNode(session::SystemPayload{});
  const session::MessageNode tool = session::MakeNode(session::ToolPayload{});

  REQUIRE(std::holds_alternative<session::UserPayload>(user.payload));
  REQUIRE(std::holds_alternative<session::AssistantPayload>(assistant.payload));
  REQUIRE(std::holds_alternative<session::SystemPayload>(system.payload));
  REQUIRE(std::holds_alternative<session::ToolPayload>(tool.payload));
}

TEST_CASE("A tool call starts pending and a completed one no longer blocks", "[context][message]") {
  session::AssistantPayload assistant;
  assistant.content = "let me check";
  assistant.reasoning.push_back(session::Reasoning{"weighing", R"(sig-1)", nullptr});
  assistant.tool_calls.push_back(
      session::ToolCallRecord{"call_1", "read_file", boost::json::object{}});

  const session::MessageNode node = session::MakeNode(std::move(assistant));

  REQUIRE(session::HasUnfinishedToolCalls(node));
  const auto& payload = std::get<session::AssistantPayload>(node.payload);
  REQUIRE(payload.reasoning.size() == 1);
  REQUIRE(payload.reasoning[0].text == "weighing");
  REQUIRE(payload.reasoning[0].signature == "sig-1");

  session::AssistantPayload done;
  done.tool_calls.push_back(session::ToolCallRecord{"call_1", "read_file", boost::json::object{},
                                                    session::ToolCallStatus::kCompleted});
  const session::MessageNode finished = session::MakeNode(std::move(done));

  REQUIRE_FALSE(session::HasUnfinishedToolCalls(finished));
}

TEST_CASE("Tool call arguments keep their JSON shape", "[context][message]") {
  session::AssistantPayload assistant;
  assistant.tool_calls.push_back(
      session::ToolCallRecord{"call_1", "read_file", boost::json::parse(R"({"path":"."})")});

  const session::MessageNode node = session::MakeNode(std::move(assistant));

  const auto& record = std::get<session::AssistantPayload>(node.payload).tool_calls.at(0);
  REQUIRE(record.arguments.is_object());
  REQUIRE(record.arguments.at("path") == ".");
}

TEST_CASE("Only a tool payload responds to a tool call", "[context][message]") {
  session::ToolPayload receipt;
  receipt.tool_call_id = "call_1";
  receipt.tool_name = "read_file";
  receipt.content = "file contents";
  const session::MessageNode node = session::MakeNode(std::move(receipt));

  const session::ToolPayload& payload = std::get<session::ToolPayload>(node.payload);
  REQUIRE(payload.tool_call_id == "call_1");
  REQUIRE(payload.tool_name == "read_file");
  REQUIRE_FALSE(session::HasUnfinishedToolCalls(node));
}

TEST_CASE("A node carries a timestamp and parent links", "[context][message]") {
  session::MessageNode node = session::MakeNode(session::UserPayload{});
  REQUIRE(node.timestamp.empty());

  node.timestamp = "2026-09-21T10:00:00Z";
  REQUIRE(node.timestamp == "2026-09-21T10:00:00Z");
  REQUIRE(node.parent.empty());
}

TEST_CASE("The shared UUID generator meets the v4 contract", "[context][message]") {
  const std::string id = uuid::Generate();
  REQUIRE(id.size() == 36);
  REQUIRE(id[14] == '4');
  REQUIRE(std::string("89ab").find(id[19]) != std::string::npos);
  REQUIRE(uuid::Generate() != id);
}

TEST_CASE("A tool call keeps its id, name, and arguments through the JSON shape",
          "[context][message]") {
  session::ToolCallRecord record{"call_1", "read_file", boost::json::parse(R"({"path":"."})")};

  const session::ToolCallRecord as_object =
      session::ToolCallFromJson(session::ToolCallToJson(record));
  REQUIRE(as_object.id == "call_1");
  REQUIRE(as_object.name == "read_file");
  REQUIRE(as_object.arguments.is_object());
  REQUIRE(as_object.arguments.at("path") == ".");

  record.arguments = boost::json::value(R"({"path":"."})");
  const session::ToolCallRecord as_string =
      session::ToolCallFromJson(session::ToolCallToJson(record));
  REQUIRE(as_string.arguments.is_string());
  REQUIRE(boost::json::value_to<std::string>(as_string.arguments) == R"({"path":"."})");
}
