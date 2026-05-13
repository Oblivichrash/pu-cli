// SPDX-License-Identifier: GPL-3.0-only

#include "backends/ollama/ollama_backend.hpp"
#include "backends/ollama/ollama_token_adapter.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace pu::backend;
using namespace pu::backends::ollama;
using namespace pu::tests;

TEST_CASE("OllamaBackend request building", "[ollama]") {
  OllamaBackend::Config config;
  config.model = "llama3.2:1b";
  config.temperature = 0.5f;
  config.system_prompt = "You are helpful.";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  auto adapter = std::make_unique<OllamaTokenAdapter>();
  OllamaBackend backend(std::move(config), std::move(mock_http), std::move(adapter));

  std::vector<pu::backend::Message> history = {
    {pu::backend::Message::Role::kUser, "Hello"}
  };

  std::error_code ec;
  backend.Chat(history, [](pu::backend::TokenType, std::string_view, bool) {}, ec);
  REQUIRE_FALSE(ec);

  auto body = nlohmann::json::parse(mock_ptr->last_body);
  REQUIRE(body["model"] == "llama3.2:1b");
  REQUIRE(body["stream"] == true);
  REQUIRE(body["options"]["temperature"] == 0.5f);
  REQUIRE(body["messages"].size() == 2);
  REQUIRE(body["messages"][0]["role"] == "system");
  REQUIRE(body["messages"][1]["role"] == "user");
}

TEST_CASE("OllamaBackend full streaming callback", "[ollama][streaming]") {
  OllamaBackend::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  std::vector<std::string> chunks = {
    R"({"message":{"content":"Hello"}})",
    R"({"message":{"content":" world"}})",
    R"({"done":true})"
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

  auto adapter = std::make_unique<OllamaTokenAdapter>();
  OllamaBackend backend(std::move(config), std::move(mock_http), std::move(adapter));

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
    if (!token.empty()) {
      accumulated += token;
    }
    if (is_final) {
      final_received = true;
    }
  }, ec);

  REQUIRE_FALSE(ec);
  REQUIRE(accumulated == "Hello world");
  REQUIRE(final_received == true);
  REQUIRE(mock_ptr->last_url == "http://localhost:11434/api/chat");
}

TEST_CASE("OllamaBackend tool calling stream", "[ollama][tools]") {
  OllamaBackend::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&,
                                    const std::string&,
                                    const std::vector<std::string>&,
                                    pu::http::WriteCallback cb,
                                    std::error_code& ec) {
    ec.clear();
    std::string data =
        R"({"message":{"content":"Running ls","tool_calls":[{"id":"1","function":{"name":"execute_bash","arguments":{"command":"ls"}}}]}})"
        + std::string("\n");
    std::string done = R"({"done":true})" + std::string("\n");
    cb(data.data(), data.size());
    cb(done.data(), done.size());
  };

  auto adapter = std::make_unique<OllamaTokenAdapter>();
  OllamaBackend backend(std::move(config), std::move(mock_http), std::move(adapter));

  std::vector<pu::backend::Message> history = {{pu::backend::Message::Role::kUser, "list files"}};
  pu::backend::ToolDefinition tool;
  tool.name = "execute_bash";
  tool.parameters.raw_schema = "{}";
  std::vector<pu::backend::ToolDefinition> tools = {tool};

  bool tool_fired = false;
  std::error_code ec;
  backend.Chat(history, tools,
    [](TokenType, std::string_view, bool) {},
    [&](const ToolCall& call) {
      tool_fired = true;
      REQUIRE(call.name == "execute_bash");
    },
    ec);
  REQUIRE_FALSE(ec);
  REQUIRE(tool_fired);
}
