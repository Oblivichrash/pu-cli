// SPDX-License-Identifier: GPL-3.0-only

#include "backends/ollama/ollama_token_adapter.hpp"
#include "backends/openai/openai_token_adapter.hpp"
#include "pu/backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

using namespace pu::backend;
using namespace pu::backends;
using json = nlohmann::json;

namespace {
struct CallRecord {
  TokenType type;
  std::string token;
  bool is_final;
  std::vector<ToolCall> tool_calls;
};
}  // namespace

static auto MakeContentRecorder(std::vector<CallRecord>& records) {
  return [&](TokenType type, std::string_view token, bool is_final) {
    records.push_back({type, std::string(token), is_final, {}});
  };
}

static auto MakeToolRecorder(std::vector<ToolCall>& tool_calls) {
  return [&](const ToolCall& call) {
    tool_calls.push_back(call);
  };
}

TEST_CASE("OllamaTokenAdapter emits content and reasoning", "[token_adapter]") {
  ollama::OllamaTokenAdapter adapter;
  std::vector<CallRecord> records;
  std::vector<ToolCall> tool_calls;
  auto content_cb = MakeContentRecorder(records);
  auto tool_cb = MakeToolRecorder(tool_calls);

  SECTION("content token") {
    json j = json::parse(R"({"message":{"content":"Hello"}})");
    adapter.HandleJson(j, content_cb, tool_cb);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].type == TokenType::kContent);
    REQUIRE(records[0].token == "Hello");
    REQUIRE(!records[0].is_final);
  }

  SECTION("reasoning token") {
    json j = json::parse(R"({"message":{"thinking":"Hmm..."}})");
    adapter.HandleJson(j, content_cb, tool_cb);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].type == TokenType::kReasoning);
    REQUIRE(records[0].token == "Hmm...");
  }

  SECTION("done signal") {
    json j = json::parse(R"({"done":true})");
    adapter.HandleJson(j, content_cb, tool_cb);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].is_final == true);
  }
}

TEST_CASE("OllamaTokenAdapter extracts tool calls", "[token_adapter]") {
  ollama::OllamaTokenAdapter adapter;
  std::vector<CallRecord> records;
  std::vector<ToolCall> tool_calls;
  auto content_cb = MakeContentRecorder(records);
  auto tool_cb = MakeToolRecorder(tool_calls);

  std::string json_str = R"({
    "message": {
      "content": "I will run ls",
      "tool_calls": [{
        "id": "call_1",
        "function": {
          "name": "execute_bash",
          "arguments": {"command": "ls"}
        }
      }]
    }
  })";
  json j = json::parse(json_str);
  adapter.HandleJson(j, content_cb, tool_cb);

  REQUIRE(records.size() == 1);
  REQUIRE(tool_calls.size() == 1);
  REQUIRE(tool_calls[0].name == "execute_bash");
  REQUIRE(tool_calls[0].arguments == R"({"command":"ls"})");
}

TEST_CASE("OpenAITokenAdapter accumulates deltas", "[token_adapter]") {
  openai::OpenAITokenAdapter adapter;
  std::vector<CallRecord> records;
  std::vector<ToolCall> tool_calls;
  auto content_cb = MakeContentRecorder(records);
  auto tool_cb = MakeToolRecorder(tool_calls);

  SECTION("content delta") {
    json j = json::parse(R"({"choices":[{"delta":{"content":"Hi"}}]})");
    adapter.HandleJson(j, content_cb, tool_cb);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].token == "Hi");
  }

  SECTION("tool call deltas are buffered") {
    json delta1 = json::parse(R"({
      "choices":[{
        "delta":{
          "tool_calls":[{"index":0, "id":"call_x", "function":{"name":"exec","arguments":"ls"}}]
        }
      }]
    })");
    adapter.HandleJson(delta1, content_cb, tool_cb);

    // Finalize with done
    json done = json::parse(R"({"done":true})");
    adapter.HandleJson(done, content_cb, tool_cb);

    REQUIRE(records.size() >= 1);
    // Final signal should produce a content record with is_final = true
    REQUIRE(records.back().is_final == true);
    REQUIRE(tool_calls.size() == 1);
    REQUIRE(tool_calls[0].arguments == "ls");
  }

  SECTION("reasoning content handled") {
    json j = json::parse(R"({"choices":[{"delta":{"reasoning_content":"Let me think"}}]})");
    adapter.HandleJson(j, content_cb, tool_cb);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].type == TokenType::kReasoning);
  }
}
