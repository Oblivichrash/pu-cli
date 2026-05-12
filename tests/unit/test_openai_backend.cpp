// SPDX-License-Identifier: GPL-3.0-only

#include "backends/openai/openai_backend.hpp"
#include "backends/openai/openai_token_adapter.hpp"
#include "backends/openai/sse_parser.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/error_codes.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <nlohmann/json.hpp>
#include <system_error>

using namespace pu::backend;
using namespace pu::backends::openai;
using namespace pu::tests;

TEST_CASE("OpenAIBackend SSE parsing", "[openai]") {
  using internal::ParseSseLine;
  using internal::SseToken;

  SECTION("extracts content from valid line") {
    std::string line = R"(data: {"choices":[{"delta":{"content":"Hello"}}]})";
    auto token = ParseSseLine(line);
    REQUIRE(token.has_value());
    REQUIRE(token->content == "Hello");
    REQUIRE(token->done == false);
  }

  SECTION("recognizes [DONE] flag") {
    std::string line = "data: [DONE]";
    auto token = ParseSseLine(line);
    REQUIRE(token.has_value());
    REQUIRE(token->done == true);
  }

  SECTION("ignores lines without data prefix") {
    std::string line = ": heartbeat";
    auto token = ParseSseLine(line);
    REQUIRE(!token.has_value());
  }

  SECTION("ignores empty content") {
    std::string line = R"(data: {"choices":[{"delta":{}}]})";
    auto token = ParseSseLine(line);
    REQUIRE(!token.has_value());
  }

  SECTION("throws on invalid JSON") {
    std::string line = "data: not json";
    REQUIRE_THROWS_AS(ParseSseLine(line), std::runtime_error);
  }

  SECTION("handles whitespace after data: prefix") {
    std::string line = R"(data:  {"choices":[{"delta":{"content":"Hello"}}]})";
    auto token = ParseSseLine(line);
    REQUIRE(token.has_value());
    REQUIRE(token->content == "Hello");
  }
}

TEST_CASE("OpenAIBackend request building", "[openai]") {
  OpenAIBackend::Config config;
  config.model = "gpt-4o-mini";
  config.temperature = 0.7f;
  config.system_prompt = "You are helpful.";
  config.host = "https://api.openai.com/v1";
  config.api_key = "test-key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  auto adapter = std::make_unique<OpenAITokenAdapter>();
  OpenAIBackend backend(config, std::move(mock_http), std::move(adapter));

  std::vector<pu::backend::Message> history = {
    {pu::backend::Message::Role::kUser, "Hello"}
  };

  std::error_code ec;
  backend.Chat(history, [](pu::backend::TokenType, std::string_view, bool) {}, ec);
  REQUIRE_FALSE(ec);

  auto body = nlohmann::json::parse(mock_ptr->last_body);
  REQUIRE(body["model"] == "gpt-4o-mini");
  REQUIRE(body["stream"] == true);
  REQUIRE(body["temperature"] == 0.7f);
  REQUIRE(body["messages"].size() == 2);
  REQUIRE(body["messages"][0]["role"] == "system");
  REQUIRE(body["messages"][1]["role"] == "user");

  bool has_auth = false;
  for (const auto& h : mock_ptr->last_headers) {
    if (h.find("Authorization: Bearer test-key") != std::string::npos) has_auth = true;
  }
  REQUIRE(has_auth);
}

TEST_CASE("OpenAIBackend does not send Authorization header when api_key is empty", "[openai]") {
  OpenAIBackend::Config config;
  config.model = "local-model";
  config.host = "http://localhost:8080/v1";
  config.api_key = "";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  auto adapter = std::make_unique<OpenAITokenAdapter>();
  OpenAIBackend backend(config, std::move(mock_http), std::move(adapter));

  std::vector<pu::backend::Message> history = {{pu::backend::Message::Role::kUser, "Hi"}};
  std::error_code ec;
  backend.Chat(history, [](auto&&...) {}, ec);
  REQUIRE_FALSE(ec);

  bool has_auth = false;
  for (const auto& h : mock_ptr->last_headers) {
    if (h.find("Authorization:") != std::string::npos) has_auth = true;
  }
  REQUIRE_FALSE(has_auth);
  REQUIRE(mock_ptr->last_url == "http://localhost:8080/v1/chat/completions");
}

TEST_CASE("OpenAIBackend full streaming callback", "[openai][streaming]") {
  OpenAIBackend::Config config;
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
                                    pu::http::WriteCallback cb,
                                    std::error_code& ec) {
    ec.clear();
    for (const auto& chunk : chunks) {
      std::string data = chunk + "\n";
      cb(data.data(), data.size());
    }
  };

  auto adapter = std::make_unique<OpenAITokenAdapter>();
  OpenAIBackend backend(config, std::move(mock_http), std::move(adapter));

  std::vector<pu::backend::Message> history = {
    {pu::backend::Message::Role::kUser, "Hi"}
  };

  std::string accumulated;
  bool final_received = false;
  std::error_code ec;

  backend.Chat(history, [&](pu::backend::TokenType type,
                            std::string_view token,
                            bool is_final) {
    REQUIRE(type == pu::backend::TokenType::kContent);
    if (!token.empty()) accumulated += token;
    if (is_final) final_received = true;
  }, ec);

  REQUIRE_FALSE(ec);
  REQUIRE(accumulated == "Hello world");
  REQUIRE(final_received == true);
  REQUIRE(mock_ptr->last_url == "https://api.openai.com/v1/chat/completions");
}

TEST_CASE("OpenAIBackend handles HTTP errors", "[openai][error]") {
  OpenAIBackend::Config config;
  config.model = "gpt-4o-mini";
  config.api_key = "invalid-key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&,
                                    const std::string&,
                                    const std::vector<std::string>&,
                                    pu::http::WriteCallback,
                                    std::error_code& ec) {
    ec = pu::HttpErrc::http_error;
  };

  auto adapter = std::make_unique<OpenAITokenAdapter>();
  OpenAIBackend backend(config, std::move(mock_http), std::move(adapter));

  std::vector<pu::backend::Message> history = {{pu::backend::Message::Role::kUser, "Hi"}};
  std::error_code ec;
  backend.Chat(history, [](auto&&...) {}, ec);
  REQUIRE(ec);
  REQUIRE(ec == pu::HttpErrc::http_error);
}

TEST_CASE("OpenAIBackend tool calling stream", "[openai][tools]") {
  OpenAIBackend::Config config;
  config.model = "gpt-4o-mini";
  config.host = "https://api.openai.com/v1";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&,
                                    const std::string&,
                                    const std::vector<std::string>&,
                                    pu::http::WriteCallback cb,
                                    std::error_code& ec) {
    ec.clear();
    std::string chunk1 =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"exec","arguments":"ls"}}]}}]})"
        + std::string("\n");
    std::string chunk2 = "data: [DONE]\n";
    cb(chunk1.data(), chunk1.size());
    cb(chunk2.data(), chunk2.size());
  };

  auto adapter = std::make_unique<OpenAITokenAdapter>();
  OpenAIBackend backend(config, std::move(mock_http), std::move(adapter));

  std::vector<pu::backend::Message> history = {{pu::backend::Message::Role::kUser, "list"}};
  pu::backend::ToolDefinition tool;
  tool.name = "exec";
  tool.parameters.raw_schema = "{}";
  std::vector<pu::backend::ToolDefinition> tools = {tool};

  bool tool_fired = false;
  std::error_code ec;
  backend.Chat(history, tools,
    [](TokenType, std::string_view, bool) {},
    [&](const ToolCall& call) {
      tool_fired = true;
      REQUIRE(call.arguments == "ls");
    },
    ec);
  REQUIRE_FALSE(ec);
  REQUIRE(tool_fired);
}
