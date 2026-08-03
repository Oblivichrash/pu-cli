// SPDX-License-Identifier: GPL-3.0-only

#include "pu/llm/openai_provider.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/error.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace pu;
using namespace pu::tests;

TEST_CASE("OpenAIProvider request building", "[openai]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";
  config.temperature = 0.7f;
  config.host = "https://api.openai.com/v1";
  config.api_key = "test-key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {
    ChatMessage{1, "now", "user", "Hello", "", ""}
  };

  provider.Chat(history, {});

  auto body = nlohmann::json::parse(mock_ptr->last_body);
  REQUIRE(body["model"] == "gpt-4o-mini");
  REQUIRE(body["stream"] == true);
  REQUIRE(body["temperature"] == 0.7f);

  bool has_auth = false;
  for (const auto& h : mock_ptr->last_headers) {
    if (h.find("Authorization: Bearer test-key") != std::string::npos) has_auth = true;
  }
  REQUIRE(has_auth);
}

TEST_CASE("OpenAIProvider does not send Authorization header when api_key is empty", "[openai]") {
  OpenAIProvider::Config config;
  config.model = "local-model";
  config.host = "http://localhost:8080/v1";
  config.api_key = "";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi", "", ""}};
  provider.Chat(history, {});

  bool has_auth = false;
  for (const auto& h : mock_ptr->last_headers) {
    if (h.find("Authorization:") != std::string::npos) has_auth = true;
  }
  REQUIRE_FALSE(has_auth);
}

TEST_CASE("OpenAIProvider full streaming callback", "[openai][streaming]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";
  config.host = "https://api.openai.com/v1";
  config.api_key = "test-key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  std::vector<std::string> chunks = {
    R"(data: {"choices":[{"delta":{"content":"Hello"}}]})",
    R"(data: {"choices":[{"delta":{"content":" world"}}]})",
    R"(data: [DONE])"
  };

  mock_ptr->simulate_response = [&](const std::string&,
                                    const std::string&,
                                    const std::vector<std::string>&,
                                    pu::http::WriteCallback cb) {
    for (const auto& chunk : chunks) {
      std::string data = chunk + "\n";
      cb(data.data(), data.size());
    }
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {
    ChatMessage{1, "now", "user", "Hi", "", ""}
  };

  std::string accumulated;
  auto result = provider.Chat(history, {},
    [&](const std::string& token) {
      accumulated += token;
    },
    [](const ToolCall&) {}
  );

  REQUIRE(result.content == "Hello world");
}

TEST_CASE("OpenAIProvider handles HTTP errors", "[openai][error]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";
  config.api_key = "invalid-key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&,
                                    const std::string&,
                                    const std::vector<std::string>&,
                                    pu::http::WriteCallback) {
    throw pu::HttpError("HTTP error 401: Unauthorized");
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi", "", ""}};
  REQUIRE_THROWS_AS(provider.Chat(history, {}), pu::HttpError);
}

TEST_CASE("OpenAIProvider tool calling stream", "[openai][tools]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";
  config.host = "https://api.openai.com/v1";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&,
                                    const std::string&,
                                    const std::vector<std::string>&,
                                    pu::http::WriteCallback cb) {
    std::string chunk1 =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"exec","arguments":"ls"}}]}}]})"
        + std::string("\n");
    std::string chunk2 = "data: [DONE]\n";
    cb(chunk1.data(), chunk1.size());
    cb(chunk2.data(), chunk2.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "list", "", ""}};
  ToolDefinition tool;
  tool.name = "exec";
  tool.parameters_schema = "{}";
  std::vector<ToolDefinition> tools = {tool};

  bool tool_fired = false;
  auto result = provider.Chat(history, tools,
    [](const std::string&) {},
    [&](const ToolCall& call) {
      tool_fired = true;
    });
  REQUIRE(tool_fired);
}

TEST_CASE("OpenAIProvider adds extra_body to disable thinking when enable_thinking=false", "[openai]") {
  OpenAIProvider::Config config;
  config.model = "deepseek-reasoner";
  config.host = "https://api.deepseek.com/v1";
  config.api_key = "test-key";
  config.enable_thinking = false;

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi", "", ""}};
  provider.Chat(history, {});

  auto body = nlohmann::json::parse(mock_ptr->last_body);
  REQUIRE(body.contains("extra_body"));
  REQUIRE(body["extra_body"]["thinking"]["type"] == "disabled");
}

TEST_CASE("OpenAIProvider omits extra_body when enable_thinking=true", "[openai]") {
  OpenAIProvider::Config config;
  config.model = "deepseek-reasoner";
  config.host = "https://api.deepseek.com/v1";
  config.api_key = "test-key";
  config.enable_thinking = true;

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi", "", ""}};
  provider.Chat(history, {});

  auto body = nlohmann::json::parse(mock_ptr->last_body);
  REQUIRE_FALSE(body.contains("extra_body"));
}

TEST_CASE("OpenAIProvider IsThinkingMode reflects enable_thinking", "[openai]") {
  OpenAIProvider::Config config;
  config.enable_thinking = true;
  auto mock_http = std::make_unique<MockHttpClient>();
  OpenAIProvider thinking(config, std::move(mock_http));
  REQUIRE(thinking.IsThinkingMode() == true);

  config.enable_thinking = false;
  mock_http = std::make_unique<MockHttpClient>();
  OpenAIProvider nothinking(config, std::move(mock_http));
  REQUIRE(nothinking.IsThinkingMode() == false);
}
