// SPDX-License-Identifier: GPL-3.0-only

#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/projection.hpp"
#include "pu/config/backend.hpp"
#include "tests/mocks/provider_harness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/json.hpp>

using namespace pu;
using namespace pu::tests;

namespace {

using Harness = ProviderHarness<OllamaProvider, OllamaProvider::Config>;

// The config every test here starts from: one model, and the daemon it is asked
// of. A test that needs something else copies this and changes that one field.
OllamaProvider::Config LocalConfig(const char* model = "llama3.2:1b",
                                   const char* host = "http://localhost:11434") {
  OllamaProvider::Config config;
  config.model = model;
  config.host = host;
  return config;
}

}  // namespace

TEST_CASE("OllamaProvider request building", "[ollama]") {
  auto config = LocalConfig();
  config.temperature = 0.5f;

  Harness harness(std::move(config));
  const auto body = harness.BodyOf("Hello");

  REQUIRE(body.at("model") == "llama3.2:1b");
  REQUIRE(body.at("stream") == true);
  REQUIRE(body.at("options").at("temperature") == 0.5f);
  REQUIRE_FALSE(body.as_object().contains("max_tokens"));
  REQUIRE_FALSE(body.at("options").as_object().contains("num_predict"));
}

TEST_CASE("Backend leaves the token cap unset until it is asked for", "[config]") {
  const auto config_from = [](const char* text) {
    return boost::json::value_to<pu::config::Backend>(boost::json::parse(text));
  };

  REQUIRE_FALSE(config_from(R"({"gateway":"ollama","model":"m"})").max_tokens.has_value());
  REQUIRE(config_from(R"({"gateway":"ollama","model":"m","max_tokens":4096})").max_tokens == 4096);

  // Round-trips keep the absent key absent rather than inventing a cap.
  const auto bare = config_from(R"({"gateway":"ollama","model":"m"})");
  const auto written = boost::json::value_from(bare).as_object();
  REQUIRE_FALSE(written.contains("max_tokens"));
}

TEST_CASE("OllamaProvider sends a thinking level as top-level think", "[ollama]") {
  const auto body_for = [](ThinkingLevel level) {
    auto config = LocalConfig();
    config.thinking = level;
    Harness harness(std::move(config));
    return harness.BodyOf("Hello");
  };

  REQUIRE(body_for(ThinkingLevel::kNone).at("think") == false);
  REQUIRE(body_for(ThinkingLevel::kLow).at("think") == "low");
  REQUIRE(body_for(ThinkingLevel::kHigh).at("think") == "high");

  const auto undecided = body_for(ThinkingLevel::kServerDefault);
  REQUIRE_FALSE(undecided.as_object().contains("think"));
}

TEST_CASE("OllamaProvider full streaming callback", "[ollama][streaming]") {
  Harness harness(LocalConfig());

  std::vector<std::string> chunks = {R"({"message":{"content":"Hello"}})",
                                     R"({"message":{"content":" world"}})", R"({"done":true})"};

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    for (const auto& chunk : chunks) {
      std::string data = chunk + "\n";
      cb(data.data(), data.size());
    }
  };

  std::string accumulated;
  const std::vector<ChatMessage> history = {ChatMessage{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}, [&](std::string_view token) {
    accumulated += token;
  }));

  REQUIRE(result.content == "Hello world");
  REQUIRE_FALSE(result.usage.has_value());
}

TEST_CASE("OllamaProvider joins consecutive thinking deltas into one block", "[ollama][reasoning]") {
  Harness harness(LocalConfig());

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    for (const char* chunk : {R"({"message":{"thinking":"Let me "}})" "\n",
                              R"({"message":{"thinking":"think."}})" "\n",
                              R"({"message":{"content":"done"},"done":true})" "\n"}) {
      std::string line = chunk;
      cb(line.data(), line.size());
    }
  };

  auto result = harness.Send();

  REQUIRE(result.reasoning.size() == 1);
  REQUIRE(result.reasoning.front().text == "Let me think.");
  REQUIRE(result.content == "done");
}

TEST_CASE("OllamaProvider reports the token counts it was sent", "[ollama][usage]") {
  Harness harness(LocalConfig());

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string chunk =
        R"({"message":{"content":"hi"}})"
        "\n"
        R"({"message":{"content":""},"done":true,"prompt_eval_count":21,"eval_count":9,"total_duration":123456,"eval_duration":789})"
        "\n";
    cb(chunk.data(), chunk.size());
  };

  auto result = harness.Send();

  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 21);
  REQUIRE(result.usage->completion_tokens == 9);

  // The final line carries timings beside the counts; they are kept as the daemon wrote
  // them rather than dropped with the line.
  REQUIRE(result.raw_usage.as_object().contains("total_duration"));
  REQUIRE(result.raw_usage.at("eval_duration") == 789);
}

TEST_CASE("OllamaProvider keeps a count it does not name", "[ollama][usage]") {
  Harness harness(LocalConfig());

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string chunk =
        R"({"message":{"content":"hi"}})"
        "\n"
        R"({"message":{"content":""},"done":true,"prompt_eval_count":21,"eval_count":9,"a_count_from_the_future":7})"
        "\n";
    cb(chunk.data(), chunk.size());
  };

  auto result = harness.Send();

  // A count this build has never heard of survives, because the line is kept whole
  // rather than filtered to a list of names that would have to be remembered.
  REQUIRE(result.raw_usage.at("a_count_from_the_future") == 7);
}

TEST_CASE("OllamaProvider names the fields on a captured frame that nothing reads",
          "[ollama][usage][fixture]") {
  Harness harness(LocalConfig("gpt-oss:120b", "https://ollama.com"));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    const std::string chunk = ReadFixture("ollama-thinking.ndjson");
    cb(const_cast<char*>(chunk.data()), chunk.size());
  };

  auto result = harness.Send();

  REQUIRE(result.usage->cached_tokens == 48);

  // `created_at` arrives on every line and nothing names it; the set is asserted whole so
  // that a field the daemon adds fails here rather than passing unread.
  REQUIRE(result.unread_fields == std::vector<std::string>{"created_at"});
}

TEST_CASE("OllamaProvider reads a captured thinking stream", "[ollama]") {
  Harness harness(LocalConfig("gpt-oss:120b", "https://ollama.com"));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    const std::string chunk = ReadFixture("ollama-thinking.ndjson");
    cb(const_cast<char*>(chunk.data()), chunk.size());
  };

  auto result = harness.Send();

  REQUIRE(result.content == "pong");
  REQUIRE(result.finish_reason == "stop");
  REQUIRE(result.model == "gpt-oss:120b");
  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 73);
  REQUIRE(result.usage->completion_tokens == 42);
  // The prompt was answered partly from the cache, and the count rides on the same line
  // under Ollama's own name rather than a usage object.
  REQUIRE(result.usage->cached_tokens == 48);
}

TEST_CASE("OllamaProvider ignores a blank line in the stream", "[ollama][streaming]") {
  Harness harness(LocalConfig());

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string chunk = R"({"message":{"content":"hi"}})"
                        "\n"
                        "\n"
                        R"({"message":{"content":""},"done":true})"
                        "\n";
    cb(chunk.data(), chunk.size());
  };

  auto result = harness.Send();

  REQUIRE(result.content == "hi");
}

TEST_CASE("OllamaProvider tool calling stream", "[ollama][tools]") {
  Harness harness(LocalConfig());

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string data =
        R"({"message":{"content":"Running ls","tool_calls":[{"function":{"name":"execute_bash","arguments":{"command":"ls"}}}]}})" +
        std::string("\n");
    std::string done = R"({"done":true})" + std::string("\n");
    cb(data.data(), data.size());
    cb(done.data(), done.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "list files"}};
  ToolDefinition tool;
  tool.name = "execute_bash";
  tool.parameters = boost::json::object{};
  std::vector<ToolDefinition> tools = {tool};

  auto result = harness->Chat(ChatRequest(history, tools, [](std::string_view) {}));

  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].name == "execute_bash");
}

TEST_CASE("OllamaProvider passes tool_call_id for tool messages", "[ollama][tools]") {
  Harness harness(LocalConfig());

  ChatMessage tool_msg;
  tool_msg.id = 1;
  tool_msg.role = "tool";
  tool_msg.content = "result";
  tool_msg.tool_name = "execute_bash";
  tool_msg.tool_call_id = "call_42";
  std::vector<ChatMessage> history = {tool_msg};

  harness->Chat(ChatRequest(history, {}));
  auto body = harness.Body();
  REQUIRE(body.at("messages").at(0).at("role") == "tool");
  REQUIRE(body.at("messages").at(0).at("tool_name") == "execute_bash");
  REQUIRE(body.at("messages").at(0).at("tool_call_id") == "call_42");
}

TEST_CASE("OllamaProvider keeps tool call names and arguments", "[ollama][tools]") {
  Harness harness(LocalConfig());

  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"ls","arguments":{"path":"."}}}])");
  std::vector<ChatMessage> history = {assistant};

  ToolDefinition tool;
  tool.name = "ls";
  tool.parameters = boost::json::object{};
  const std::vector<ToolDefinition> tools = {tool};
  harness->Chat(ChatRequest(history, tools));

  auto body = harness.Body();
  auto& sent = body.at("messages").at(0).at("tool_calls").as_array()[0];
  REQUIRE(sent.at("id") == "call_1");
  REQUIRE(sent.at("function").at("name") == "ls");
  REQUIRE(sent.at("function").at("arguments").at("path") == ".");
}

TEST_CASE("OllamaProvider decodes arguments sent as a JSON string", "[ollama][tools]") {
  Harness harness(LocalConfig());

  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","function":{"name":"ls","arguments":"{\"path\":\".\"}"}}])");
  std::vector<ChatMessage> history = {assistant};

  ToolDefinition tool;
  tool.name = "ls";
  tool.parameters = boost::json::object{};
  const std::vector<ToolDefinition> tools = {tool};
  harness->Chat(ChatRequest(history, tools));

  auto body = harness.Body();
  auto& sent = body.at("messages").at(0).at("tool_calls").as_array()[0];
  REQUIRE(sent.at("function").at("arguments").is_object());
  REQUIRE(sent.at("function").at("arguments").at("path") == ".");
}

TEST_CASE("OllamaProvider reports why the reply stopped", "[ollama][streaming]") {
  Harness harness(LocalConfig());

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string chunk =
        R"({"message":{"content":"half"},"done":true,"done_reason":"length"})" + std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  auto result = harness.Send("write a lot");

  REQUIRE(result.content == "half");
  REQUIRE(result.finish_reason == "length");
}

TEST_CASE("OllamaProvider keeps the reasoning a thinking model reports", "[ollama][streaming]") {
  Harness harness(LocalConfig("deepseek-r1:7b"));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string chunk =
        R"({"message":{"content":"","thinking":"weighing the options"},"done":true})" +
        std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  auto result = harness.Send("think about it");

  REQUIRE(llm::ProjectReasoningText(result.reasoning) == "weighing the options");
  REQUIRE(result.content.empty());
}

TEST_CASE("Reasoning fragments coalesce into one block, a signed one starts a new block",
          "[ollama][streaming]") {
  Harness harness(LocalConfig("deepseek-r1:7b"));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    for (const char* fragment : {R"({"message":{"thinking":"weigh"}})",
                                 R"({"message":{"thinking":"ing"}})"}) {
      std::string chunk = std::string(fragment) + "\n";
      cb(chunk.data(), chunk.size());
    }
    std::string last = R"({"message":{"thinking":"done"},"done":true})" + std::string("\n");
    cb(last.data(), last.size());
  };

  auto result = harness.Send("think");

  REQUIRE(result.reasoning.size() == 1);
  REQUIRE(result.reasoning[0].text == "weighingdone");
}

TEST_CASE("OllamaProvider raises an error sent inside the stream", "[ollama][error]") {
  Harness harness(LocalConfig("missing-model"));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string chunk = R"({"error":"model not found"})" + std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  try {
    harness.Send();
    FAIL("an error inside the stream should reach the caller");
  } catch (const std::exception& e) {
    REQUIRE(std::string(e.what()).find("model not found") != std::string::npos);
  }
}

TEST_CASE("OllamaProvider reports the model that answered", "[ollama][streaming]") {
  // A tag, which the daemon resolves to a build: the test asserts the build it
  // named back, not the tag it was asked for.
  Harness harness(LocalConfig("llama3.2"));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string chunk =
        std::string(R"({"model":"llama3.2:1b","message":{"content":"hi"},"done":false})") + "\n" +
        std::string(
            R"({"model":"llama3.2:1b","message":{"content":""},"done":true,"done_reason":"stop"})") +
        "\n";
    cb(chunk.data(), chunk.size());
  };

  auto result = harness.Send();

  REQUIRE(result.model == "llama3.2:1b");
}

TEST_CASE("OllamaProvider hands reasoning to the caller as it arrives", "[ollama][streaming]") {
  Harness harness(LocalConfig("deepseek-r1:7b"));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                         const std::vector<std::string>&,
                                         pu::http::WriteCallback cb) {
    std::string chunk = R"({"message":{"content":"","thinking":"weighing "}})"
                        "\n"
                        R"({"message":{"content":"","thinking":"the options"}})"
                        "\n"
                        R"({"message":{"content":"answer"},"done":true,"done_reason":"stop"})"
                        "\n";
    cb(chunk.data(), chunk.size());
  };

  std::string streamed;
  std::string content;
  const std::vector<ChatMessage> history = {{1, "now", "user", "think"}};
  ChatRequest request(history, {});
  request.on_content = [&](std::string_view token) { content += token; };
  request.on_reasoning = [&](std::string_view token) { streamed += token; };
  auto result = harness->Chat(request);

  REQUIRE(streamed == "weighing the options");
  REQUIRE(content == "answer");
  REQUIRE(llm::ProjectReasoningText(result.reasoning) == "weighing the options");
}
