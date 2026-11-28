// SPDX-License-Identifier: GPL-3.0-only

#include "pu/llm/anthropic_provider.hpp"

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "tests/mocks/provider_harness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace pu;
using namespace pu::tests;

namespace {

using Harness = ProviderHarness<AnthropicProvider, AnthropicProvider::Config>;

AnthropicProvider::Config RemoteConfig() {
  AnthropicProvider::Config config;
  config.model = "claude-sonnet-4-20250514";
  config.api_key = "test-key";
  config.max_tokens = 1024;
  return config;
}

// Play a fixture's frames through the mock as one reply, the way the transport hands them
// over: the fixture is the bytes, so what is tested is the reader rather than a rewrite.
void PlayFixture(MockHttpClient& mock, const std::string& name) {
  const std::string chunk = ReadFixture(name);
  mock.simulate_response = [chunk](const std::string&, const std::string&,
                                   const std::vector<std::string>&, pu::http::WriteCallback cb) {
    cb(const_cast<char*>(chunk.data()), chunk.size());
  };
}

bool HasHeader(const std::vector<std::string>& headers, const std::string& wanted) {
  for (const std::string& header : headers) {
    if (header == wanted) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("AnthropicProvider sends its key under x-api-key, not as a bearer", "[anthropic]") {
  Harness harness(RemoteConfig());
  harness.Send();

  REQUIRE(HasHeader(harness.Mock().last_headers, "x-api-key: test-key"));
  REQUIRE(HasHeader(harness.Mock().last_headers, "anthropic-version: 2023-06-01"));
  bool has_bearer = false;
  for (const std::string& header : harness.Mock().last_headers) {
    if (header.rfind("Authorization: Bearer", 0) == 0) has_bearer = true;
  }
  REQUIRE_FALSE(has_bearer);
  REQUIRE(harness.Mock().last_url == "https://api.anthropic.com/v1/messages");
}

TEST_CASE("AnthropicProvider always sends max_tokens", "[anthropic]") {
  // This API refuses a request without a cap, so the field is present even when the caller
  // never named one.
  AnthropicProvider::Config config = RemoteConfig();
  config.max_tokens = 512;

  Harness harness(std::move(config));
  const auto body = harness.BodyOf("Hello");

  REQUIRE(body.at("model") == "claude-sonnet-4-20250514");
  REQUIRE(body.at("stream") == true);
  REQUIRE(body.at("max_tokens") == 512);
}

TEST_CASE("AnthropicProvider projects content as a block list", "[anthropic]") {
  Harness harness(RemoteConfig());
  const auto body = harness.BodyOf("Hello");

  const auto& messages = body.at("messages").as_array();
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().at("role") == "user");

  // A string in memory is a `text` block on this wire.
  const auto& blocks = messages.front().at("content").as_array();
  REQUIRE(blocks.size() == 1);
  REQUIRE(blocks.front().at("type") == "text");
  REQUIRE(blocks.front().at("text") == "Hello");
}

TEST_CASE("AnthropicProvider sends the system prompt as a field, not a message", "[anthropic]") {
  AnthropicProvider::Config config = RemoteConfig();
  config.system_prompt = "You are terse.";

  Harness harness(std::move(config));
  ChatMessage rules;
  rules.id = 1;
  rules.role = "system";
  rules.SetText("These are the rules.");
  ChatMessage hello;
  hello.id = 2;
  hello.role = "user";
  hello.SetText("Hello");
  std::vector<ChatMessage> history = {rules, hello};
  harness->Chat(ChatRequest(history, {}));
  const auto body = harness.Body();

  REQUIRE(body.at("system") == "You are terse.");
  // The message with that role is folded into the field rather than sent as one, because a
  // message with `role: "system"` is refused here.
  const auto& messages = body.at("messages").as_array();
  REQUIRE(messages.size() == 1);
  REQUIRE(messages.front().at("role") == "user");
}

TEST_CASE("AnthropicProvider turns a tool message into a tool_result block on a user turn",
          "[anthropic]") {
  Harness harness(RemoteConfig());
  ChatMessage read;
  read.id = 1;
  read.role = "user";
  read.SetText("Read the file.");
  ChatMessage contents;
  contents.id = 2;
  contents.role = "tool";
  contents.SetText("int main() {}");
  contents.tool_call_id = "toolu_01";
  std::vector<ChatMessage> history = {read, contents};
  harness->Chat(ChatRequest(history, {}));
  const auto body = harness.Body();

  const auto& messages = body.at("messages").as_array();
  REQUIRE(messages.size() == 2);
  const auto& answer = messages[1];
  // This API has no `tool` role: the result is a block on a user turn that names the call.
  REQUIRE(answer.at("role") == "user");
  const auto& block = answer.at("content").as_array().front();
  REQUIRE(block.at("type") == "tool_result");
  REQUIRE(block.at("tool_use_id") == "toolu_01");
  REQUIRE(block.at("content") == "int main() {}");
}

TEST_CASE("AnthropicProvider reads a text reply from its block events", "[anthropic][fixture]") {
  Harness harness(RemoteConfig());
  PlayFixture(harness.Mock(), "anthropic-text.sse");

  const ChatResult result = harness.Send();

  REQUIRE(result.content == "The capital of France is Paris.");
  REQUIRE(result.finish_reason == "end_turn");
  REQUIRE(result.model == "claude-sonnet-4-20250514");
  // The input count is stated on `message_start` and the output count on `message_delta`;
  // both land in the one record.
  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 14);
  REQUIRE(result.usage->completion_tokens == 11);
  // This API never states a total, so none is invented.
  REQUIRE(result.usage->total_tokens == 0);
}

TEST_CASE("AnthropicProvider accumulates a tool call from input_json_delta", "[anthropic][fixture]") {
  Harness harness(RemoteConfig());
  PlayFixture(harness.Mock(), "anthropic-tool-use.sse");

  const ChatResult result = harness.Send();

  REQUIRE(result.finish_reason == "tool_use");
  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls.front().id == "toolu_01");
  REQUIRE(result.tool_calls.front().name == "read_file");
  // The input arrives as fragments that concatenate to one object, and it is parsed back into
  // the object the rest of this tree reads rather than left as the string it came as.
  REQUIRE(result.tool_calls.front().arguments.is_object());
  REQUIRE(result.tool_calls.front().arguments.at("path") == "main.cpp");
}

TEST_CASE("AnthropicProvider keeps a thinking block and its signature", "[anthropic][fixture]") {
  Harness harness(RemoteConfig());
  PlayFixture(harness.Mock(), "anthropic-tool-use.sse");

  const ChatResult result = harness.Send();

  REQUIRE(result.reasoning.size() == 1);
  REQUIRE(result.reasoning.front().text == "Let me check the file.");
  // The signature arrives after the text and has to be replayed whole on the next turn.
  REQUIRE(result.reasoning.front().signature == "EqQBCgIYAhIM");
}

TEST_CASE("AnthropicProvider raises a reply that carries an error event", "[anthropic]") {
  Harness harness(RemoteConfig());
  const std::string chunk =
      "event: error\n"
      "data: {\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\","
      "\"message\":\"Overloaded\"}}\n\n";
  harness.Mock().simulate_response = [chunk](const std::string&, const std::string&,
                                             const std::vector<std::string>&,
                                             pu::http::WriteCallback cb) {
    cb(const_cast<char*>(chunk.data()), chunk.size());
  };

  // The failure is a frame of its own here rather than a status on the response, so it is
  // raised from the parse rather than from the transport.
  REQUIRE_THROWS_AS(harness.Send(), GenericError);
}

TEST_CASE("AnthropicProvider drops temperature when thinking is on", "[anthropic]") {
  AnthropicProvider::Config config = RemoteConfig();
  config.temperature = 0.9f;
  config.thinking = ThinkingLevel::kMedium;

  Harness harness(std::move(config));
  const auto body = harness.BodyOf("Hello");

  // This API refuses both together, and the level the caller named outranks the default
  // temperature.
  REQUIRE_FALSE(body.as_object().contains("temperature"));
  REQUIRE(body.at("thinking").at("type") == "enabled");
  REQUIRE(body.at("thinking").at("budget_tokens") == 4096);
}

TEST_CASE("AnthropicProvider sends temperature when thinking is not on", "[anthropic]") {
  AnthropicProvider::Config config = RemoteConfig();
  config.temperature = 0.9f;

  Harness harness(std::move(config));
  const auto body = harness.BodyOf("Hello");

  REQUIRE(body.at("temperature") == 0.9f);
  REQUIRE_FALSE(body.as_object().contains("thinking"));
}

TEST_CASE("AnthropicProvider names stop sequences in its own spelling", "[anthropic]") {
  AnthropicProvider::Config config = RemoteConfig();
  config.stop = {"END", "STOP"};

  Harness harness(std::move(config));
  const auto body = harness.BodyOf("Hello");

  // `stop_sequences` rather than `stop`, which is the OpenAI name for the same idea.
  const auto& stops = body.at("stop_sequences").as_array();
  REQUIRE(stops.size() == 2);
  REQUIRE(stops[0] == "END");
  REQUIRE(stops[1] == "STOP");
  REQUIRE_FALSE(body.as_object().contains("stop"));
}

TEST_CASE("AnthropicProvider sends tools with input_schema and no type wrapper", "[anthropic]") {
  Harness harness(RemoteConfig());
  ChatMessage read;
  read.id = 1;
  read.role = "user";
  read.SetText("Read a file.");
  std::vector<ChatMessage> history = {read};
  std::vector<ToolDefinition> tools = {
      ToolDefinition{"read_file", "Read a file", boost::json::value{{"type", "object"}}},
  };
  harness->Chat(ChatRequest(history, tools));
  const auto body = harness.Body();

  const auto& sent = body.at("tools").as_array();
  REQUIRE(sent.size() == 1);
  REQUIRE(sent.front().at("name") == "read_file");
  // The schema rides under `input_schema`, and there is no `{"type": "function"}` wrapper to
  // unwrap: the shape differs rather than the names inside it.
  REQUIRE(sent.front().at("input_schema").at("type") == "object");
  REQUIRE_FALSE(sent.front().as_object().contains("type"));
  REQUIRE_FALSE(sent.front().as_object().contains("function"));
}
