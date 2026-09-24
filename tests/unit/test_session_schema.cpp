// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/session/session.hpp"

#include <boost/json.hpp>

#include <memory>
#include <string>

using namespace pu;

namespace {

// What a release before the DAG refactor wrote: a flat list of messages and no
// version field.
boost::json::value LegacySession() {
  return boost::json::parse(R"({
    "workspace": {
      "history": [
        {"id": 1, "timestamp": "2026-01-01T00:00:00Z", "role": "user", "content": "hi"}
      ]
    },
    "runtime_spec": {"agent_name": "chat"}
  })");
}

}  // namespace

TEST_CASE("A session without a version field is refused", "[session][schema]") {
  REQUIRE(Session::Deserialize(LegacySession()) == nullptr);
}

TEST_CASE("A session with another version is refused", "[session][schema]") {
  boost::json::value j = LegacySession();
  j.as_object()["schema_version"] = context::kSchemaVersion - 1;
  REQUIRE(Session::Deserialize(j) == nullptr);

  boost::json::value future = LegacySession();
  future.as_object()["schema_version"] = context::kSchemaVersion + 1;
  REQUIRE(Session::Deserialize(future) == nullptr);
}

TEST_CASE("A version alone is not enough without node storage", "[session][schema]") {
  // The layout an unreachable branch used: the right number, a list of messages.
  boost::json::value j = LegacySession();
  j.as_object()["schema_version"] = context::kSchemaVersion;
  REQUIRE(Session::Deserialize(j) == nullptr);
}

TEST_CASE("A session written by this version loads", "[session][schema]") {
  Session session;
  session.GetWorkspace().Append("user", "hello");
  session.GetWorkspace().Append("assistant", "hi");

  const boost::json::value saved = session.Serialize();
  REQUIRE(saved.at("schema_version") == context::kSchemaVersion);
  REQUIRE(saved.at("workspace").at("history").is_object());

  auto restored = Session::Deserialize(boost::json::parse(boost::json::serialize(saved)));
  REQUIRE(restored != nullptr);
  REQUIRE(restored->GetWorkspace().HistorySize() == 2);
  REQUIRE(restored->GetWorkspace().GetHistory()[1].content == "hi");
}

TEST_CASE("A payload stores content as one string and reasoning as raw JSON", "[session][schema]") {
  Session session;
  session.GetWorkspace().Append("user", "hello");

  ChatMessage assistant;
  assistant.role = context::kAssistantRole;
  assistant.content = "checking";
  assistant.reasoning_content = R"({"raw":true})";
  session.GetWorkspace().Append(assistant);

  boost::json::value saved = session.Serialize();
  const boost::json::array& nodes =
      saved.at("workspace").at("history").as_object()["nodes"].as_array();
  REQUIRE(nodes.size() == 2);

  // Nodes are ordered by id, so the payload is found by the content it carries.
  const boost::json::value* assistant_node = nullptr;
  for (const boost::json::value& node : nodes) {
    REQUIRE(node.at("content").is_string());
    if (node.at("content") == "checking") assistant_node = &node;
  }

  REQUIRE(assistant_node != nullptr);
  REQUIRE(assistant_node->at("reasoning").as_object().size() == 1);
  REQUIRE(assistant_node->at("reasoning").at("raw_json") == R"({"raw":true})");
}

TEST_CASE("Every role survives a save and load", "[session][schema]") {
  Session session;
  Workspace& ws = session.GetWorkspace();

  ws.Append("user", "question");

  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.content = "checking";
  assistant.reasoning_content = "because";
  assistant.tool_calls =
      boost::json::parse(R"([{"id":"call_1","function":{"name":"ls","arguments":{"path":"."}}}])");
  ws.Append(assistant);

  ChatMessage receipt;
  receipt.role = "tool";
  receipt.tool_name = "ls";
  receipt.tool_call_id = "call_1";
  receipt.content = "file.txt";
  ws.Append(receipt);

  ChatMessage note;
  note.role = "system";
  note.content = "a note from the caller";
  ws.Append(note);

  auto restored =
      Session::Deserialize(boost::json::parse(boost::json::serialize(session.Serialize())));
  REQUIRE(restored != nullptr);

  const std::vector<ChatMessage> history = restored->GetWorkspace().GetHistory();
  REQUIRE(history.size() == 4);
  REQUIRE(history[0].role == "user");
  REQUIRE(history[1].role == "assistant");
  REQUIRE(history[1].reasoning_content == "because");
  REQUIRE(history[1].tool_calls.as_array()[0].at("function").at("name") == "ls");
  REQUIRE(history[1].tool_calls.as_array()[0].at("function").at("arguments").at("path") == ".");
  REQUIRE(history[2].role == "tool");
  REQUIRE(history[2].tool_name == "ls");
  REQUIRE(history[2].tool_call_id == "call_1");
  REQUIRE(history[3].role == "system");
}

TEST_CASE("A tool call keeps its completed status across a save", "[session][schema]") {
  Session session;
  Workspace& ws = session.GetWorkspace();

  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls =
      boost::json::parse(R"([{"id":"call_1","function":{"name":"ls","arguments":{}}}])");
  ws.Append(assistant);

  ChatMessage receipt;
  receipt.role = "tool";
  receipt.tool_call_id = "call_1";
  ws.Append(receipt);
  REQUIRE_FALSE(ws.HasPendingToolCalls());

  auto restored =
      Session::Deserialize(boost::json::parse(boost::json::serialize(session.Serialize())));
  REQUIRE(restored != nullptr);

  // Status is stored, so a reload does not resurrect a finished call.
  REQUIRE_FALSE(restored->GetWorkspace().HasPendingToolCalls());
  REQUIRE(restored->GetWorkspace().HistorySize() == 2);
}

TEST_CASE("Parents survive a save and load", "[session][schema]") {
  Session session;
  Workspace& ws = session.GetWorkspace();
  ws.Append("user", "one");
  ws.Append("assistant", "two");
  ws.Append("user", "three");

  // The chain is reconstructed from links, so each node but the first has to
  // name its parent in the file. Nodes are ordered by id, not by conversation
  // order, so the check finds them by content.
  const boost::json::value saved = session.Serialize();
  const boost::json::array& nodes = saved.at("workspace").at("history").at("nodes").as_array();
  REQUIRE(nodes.size() == 3);

  const auto node_with_text = [&](const std::string& text) -> const boost::json::value& {
    for (const boost::json::value& node : nodes) {
      if (boost::json::value_to<std::string>(node.at("content")) == text) {
        return node;
      }
    }
    return nodes.at(0);
  };
  const auto id_of = [](const boost::json::value& node) {
    return boost::json::value_to<std::string>(node.at("id"));
  };

  REQUIRE(node_with_text("one").at("parents").as_array().empty());
  REQUIRE(node_with_text("two").at("parents").as_array().size() == 1);
  REQUIRE(boost::json::value_to<std::string>(node_with_text("two").at("parents").as_array().at(
              0)) == id_of(node_with_text("one")));
  REQUIRE(boost::json::value_to<std::string>(node_with_text("three").at("parents").as_array().at(
              0)) == id_of(node_with_text("two")));

  auto restored = Session::Deserialize(boost::json::parse(boost::json::serialize(saved)));
  REQUIRE(restored != nullptr);
  const std::vector<ChatMessage> history = restored->GetWorkspace().GetHistory();
  REQUIRE(history.size() == 3);
  REQUIRE(history[0].content == "one");
  REQUIRE(history[1].content == "two");
  REQUIRE(history[2].content == "three");
}

TEST_CASE("A leaf naming no node is refused", "[session][schema]") {
  Session session;
  session.GetWorkspace().Append("user", "hello");

  boost::json::value saved = session.Serialize();
  saved.at("workspace").at("history").as_object()["leaf"] = "not-a-node-id";

  REQUIRE(Session::Deserialize(saved) == nullptr);
}

// A file can carry the right version and the right section names while a field
// inside holds something else than the stored type. Reading it must reach the
// same refusal as any other foreign layout, because the alternative is a
// conversion failure that escapes the loader and stops the program from starting.
TEST_CASE("A node whose id is not a name is refused", "[session][schema]") {
  Session session;
  session.GetWorkspace().Append("user", "hello");

  boost::json::value saved = session.Serialize();
  saved.at("workspace").at("history").as_object()["nodes"].as_array().at(0).as_object()["id"] = 123;

  REQUIRE(Session::Deserialize(saved) == nullptr);
}

TEST_CASE("A parent that is not a name is refused", "[session][schema]") {
  Session session;
  session.GetWorkspace().Append("user", "one");
  session.GetWorkspace().Append("assistant", "two");

  boost::json::value saved = session.Serialize();
  saved.at("workspace").at("history").as_object()["nodes"].as_array().at(1).as_object()["parents"] =
      boost::json::array{123};

  REQUIRE(Session::Deserialize(saved) == nullptr);
}

TEST_CASE("A version that is not a number is refused", "[session][schema]") {
  Session session;
  session.GetWorkspace().Append("user", "hello");

  boost::json::value saved = session.Serialize();
  saved.as_object()["schema_version"] = std::to_string(context::kSchemaVersion);

  REQUIRE(Session::Deserialize(saved) == nullptr);
}

TEST_CASE("A session without a runtime section is refused", "[session][schema]") {
  Session session;
  session.GetWorkspace().Append("user", "hello");

  boost::json::value saved = session.Serialize();
  saved.as_object().erase("runtime_spec");
  REQUIRE(Session::Deserialize(saved) == nullptr);

  saved.as_object()["runtime_spec"] = "openai";
  REQUIRE(Session::Deserialize(saved) == nullptr);
}
