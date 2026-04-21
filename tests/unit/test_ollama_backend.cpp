// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "backends/ollama/ollama_backend.hpp"
#include "backends/ollama/sse_parser.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

using namespace pu::backend;
using namespace pu::backends::ollama;
using namespace pu::tests;

TEST_CASE("OllamaBackend SSE parsing", "[ollama]") {
  using internal::ParseSseLine;
  using internal::SseToken;

  SECTION("extracts content from valid line") {
    std::string line = R"({"message":{"content":"Hello"}})";
    auto token = ParseSseLine(line);
    REQUIRE(token.has_value());
    REQUIRE(token->content == "Hello");
    REQUIRE(token->done == false);
  }

  SECTION("recognizes done flag") {
    std::string line = R"({"done":true})";
    auto token = ParseSseLine(line);
    REQUIRE(token.has_value());
    REQUIRE(token->done == true);
  }

  SECTION("ignores heartbeat lines") {
    std::string line = R"({"other":"field"})";
    auto token = ParseSseLine(line);
    REQUIRE(!token.has_value());
  }

  SECTION("throws on invalid JSON") {
    std::string line = "not json";
    REQUIRE_THROWS_AS(ParseSseLine(line), std::runtime_error);
  }
}

TEST_CASE("OllamaBackend request building", "[ollama]") {
  OllamaBackend::Config config;
  config.model = "llama3.2:1b";
  config.temperature = 0.5f;
  config.system_prompt = "You are helpful.";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OllamaBackend backend(std::move(config), std::move(mock_http));

  std::vector<pu::backend::Message> history = {
    {pu::backend::Message::Role::kUser, "Hello"}
  };

  backend.Chat(history, [](pu::backend::TokenType, std::string_view, bool) {});

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

  // Prepare simulated SSE stream
  std::vector<std::string> chunks = {
    R"({"message":{"content":"Hello"}})",
    R"({"message":{"content":" world"}})",
    R"({"done":true})"
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

  OllamaBackend backend(std::move(config), std::move(mock_http));

  std::vector<pu::backend::Message> history = {
    {pu::backend::Message::Role::kUser, "Hi"}
  };

  std::string accumulated;
  bool final_received = false;

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
  });

  REQUIRE(accumulated == "Hello world");
  REQUIRE(final_received == true);
  REQUIRE(mock_ptr->last_url == "http://localhost:11434/api/chat");
}
