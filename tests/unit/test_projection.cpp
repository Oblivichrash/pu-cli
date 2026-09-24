// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/llm/projection.hpp"

#include "pu/core/json.hpp"

#include <boost/json.hpp>

#include <string>
#include <vector>

using namespace pu;

namespace {

llm::ProviderCapabilities OpenAiLike() {
  return {
      .role_naming = llm::RoleNaming::kAliasToolResult,
      .echo_reasoning_content = true,
      .allows_content_with_tool_calls = false,
      .tool_arguments = llm::ToolArgumentsEncoding::kJsonString,
      .tool_calls_carry_type = true,
      .sends_tool_name = false,
      .omits_empty_tool_call_id = false,
  };
}

llm::ProviderCapabilities OllamaLike() {
  return {
      .role_naming = llm::RoleNaming::kKnownRolesOnly,
      .echo_reasoning_content = false,
      .allows_content_with_tool_calls = true,
      .tool_arguments = llm::ToolArgumentsEncoding::kJsonObject,
      .tool_calls_carry_type = false,
      .sends_tool_name = true,
      .omits_empty_tool_call_id = true,
  };
}

ChatMessage Text(const std::string& role, const std::string& content) {
  ChatMessage msg;
  msg.role = role;
  msg.content = content;
  return msg;
}

ChatMessage AssistantWithCall() {
  ChatMessage msg;
  msg.role = "assistant";
  msg.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"ls","arguments":{"path":"."}}}])");
  return msg;
}

}  // namespace

TEST_CASE("Role naming differs by capability", "[projection]") {
  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(Text("tool_result", ""), OpenAiLike()).at("role")) == "tool");
  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(Text("tool_result", ""), OllamaLike()).at("role")) == "user");

  // A known role passes through both.
  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(Text("assistant", ""), OpenAiLike()).at("role")) == "assistant");
  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(Text("assistant", ""), OllamaLike()).at("role")) == "assistant");

  // An unknown role is only corrected where the naming is a closed set.
  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(Text("custom", ""), OpenAiLike()).at("role")) == "custom");
  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(Text("custom", ""), OllamaLike()).at("role")) == "user");
}

TEST_CASE("Content beside tool calls follows the capability", "[projection]") {
  const ChatMessage assistant = AssistantWithCall();

  const boost::json::value openai = llm::ProjectMessage(assistant, OpenAiLike());
  REQUIRE(openai.at("content").is_null());

  const boost::json::value ollama = llm::ProjectMessage(assistant, OllamaLike());
  REQUIRE(ollama.at("content").is_string());
  REQUIRE(boost::json::value_to<std::string>(ollama.at("content")).empty());

  // Without tool calls both keep the text.
  const ChatMessage plain = Text("assistant", "hello");
  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(plain, OpenAiLike()).at("content")) == "hello");
  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(plain, OllamaLike()).at("content")) == "hello");
}

TEST_CASE("Reasoning is echoed only where supported", "[projection]") {
  ChatMessage assistant = Text("assistant", "answer");
  assistant.reasoning_content = "because";

  REQUIRE(boost::json::value_to<std::string>(
              llm::ProjectMessage(assistant, OpenAiLike()).at("reasoning_content")) == "because");
  REQUIRE_FALSE(json::HasKey(llm::ProjectMessage(assistant, OllamaLike()), "reasoning_content"));

  // Never attached to a non-assistant message.
  ChatMessage user = Text("user", "question");
  user.reasoning_content = "because";
  REQUIRE_FALSE(json::HasKey(llm::ProjectMessage(user, OpenAiLike()), "reasoning_content"));
}

TEST_CASE("Tool call envelope and arguments differ by capability", "[projection]") {
  const ChatMessage assistant = AssistantWithCall();

  const boost::json::value openai = llm::ProjectMessage(assistant, OpenAiLike());
  const boost::json::value& openai_call = openai.at("tool_calls").as_array().at(0);
  REQUIRE(openai_call.at("type") == "function");
  REQUIRE(openai_call.at("function").at("name") == "ls");
  REQUIRE(openai_call.at("function").at("arguments").is_string());
  REQUIRE(boost::json::parse(
              boost::json::value_to<std::string>(openai_call.at("function").at("arguments")))
              .at("path") == ".");

  const boost::json::value ollama = llm::ProjectMessage(assistant, OllamaLike());
  const boost::json::value& ollama_call = ollama.at("tool_calls").as_array().at(0);
  REQUIRE_FALSE(json::HasKey(ollama_call, "type"));
  REQUIRE(ollama_call.at("function").at("name") == "ls");
  REQUIRE(ollama_call.at("function").at("arguments").is_object());
  REQUIRE(ollama_call.at("function").at("arguments").at("path") == ".");
}

TEST_CASE("Arguments already in the target form are left alone", "[projection]") {
  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","function":{"name":"ls","arguments":"{\"path\":\".\"}"}}])");

  // An object-shaped capability parses the string, a string-shaped one keeps it.
  REQUIRE(llm::ProjectMessage(assistant, OllamaLike())
              .at("tool_calls")
              .as_array()
              .at(0)
              .at("function")
              .at("arguments")
              .is_object());
  REQUIRE(llm::ProjectMessage(assistant, OpenAiLike())
              .at("tool_calls")
              .as_array()
              .at(0)
              .at("function")
              .at("arguments")
              .is_string());
}

TEST_CASE("Unparseable arguments survive", "[projection]") {
  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls =
      boost::json::parse(R"([{"id":"call_1","function":{"name":"ls","arguments":"not json"}}])");

  const boost::json::value projected = llm::ProjectMessage(assistant, OllamaLike());
  REQUIRE(boost::json::value_to<std::string>(
              projected.at("tool_calls").as_array().at(0).at("function").at("arguments")) ==
          "not json");
}

TEST_CASE("A tool call without a nested function passes through", "[projection]") {
  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::parse(R"([{"id":"call_1","name":"flat"}])");

  const boost::json::value projected = llm::ProjectMessage(assistant, OllamaLike());
  REQUIRE(projected.at("tool_calls").as_array().at(0).at("name") == "flat");
}

TEST_CASE("Tool result fields follow the capability", "[projection]") {
  ChatMessage receipt = Text("tool", "done");
  receipt.tool_name = "ls";
  receipt.tool_call_id = "call_1";

  const boost::json::value openai = llm::ProjectMessage(receipt, OpenAiLike());
  REQUIRE_FALSE(json::HasKey(openai, "tool_name"));
  REQUIRE(openai.at("tool_call_id") == "call_1");

  const boost::json::value ollama = llm::ProjectMessage(receipt, OllamaLike());
  REQUIRE(ollama.at("tool_name") == "ls");
  REQUIRE(ollama.at("tool_call_id") == "call_1");

  // An empty id is dropped only where the capability says so.
  ChatMessage nameless = Text("tool", "done");
  REQUIRE(llm::ProjectMessage(nameless, OpenAiLike()).at("tool_call_id") == "");
  REQUIRE_FALSE(json::HasKey(llm::ProjectMessage(nameless, OllamaLike()), "tool_call_id"));
}

TEST_CASE("ProjectMessages keeps the conversation order", "[projection]") {
  const std::vector<ChatMessage> history = {
      Text("system", "be brief"),
      Text("user", "one"),
      Text("assistant", "two"),
  };

  const boost::json::array projected = llm::ProjectMessages(history, OpenAiLike());
  REQUIRE(projected.size() == 3);
  REQUIRE(projected.at(0).at("role") == "system");
  REQUIRE(projected.at(1).at("content") == "one");
  REQUIRE(projected.at(2).at("content") == "two");
  REQUIRE(llm::ProjectMessages({}, OpenAiLike()).empty());
}
