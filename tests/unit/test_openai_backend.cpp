// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "backends/openai/openai_backend.hpp"
#include "backends/openai/sse_parser.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

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
    REQUIRE(token.has_value());
    REQUIRE(token->content.empty());
    REQUIRE(token->done == false);
  }

  SECTION("throws on invalid JSON") {
    std::string line = "data: not json";
    REQUIRE_THROWS_AS(ParseSseLine(line), std::runtime_error);
  }
}

TEST_CASE("OpenAIBackend request building", "[openai]") {
  OpenAIBackend::Config config;
  config.model = "gpt-4o-mini";
  config.temperature = 0.7f;
  config.system_prompt = "You are helpful.";
  config.host = "https://api.openai.com";
  config.api_key = "test-key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OpenAIBackend backend(std::move(config), std::move(mock_http));

  std::vector<pu::backend::Message> history = {
    {pu::backend::Message::Role::kUser, "Hello"}
  };

  backend.Chat(history, [](pu::backend::TokenType, std::string_view, bool) {});

  auto body = nlohmann::json::parse(mock_ptr->last_body);
  REQUIRE(body["model"] == "gpt-4o-mini");
  REQUIRE(body["stream"] == true);
  REQUIRE(body["temperature"] == 0.7f);
  REQUIRE(body["messages"].size() == 2);
  REQUIRE(body["messages"][0]["role"] == "system");
  REQUIRE(body["messages"][1]["role"] == "user");

  // Check headers
  bool has_auth = false;
  for (const auto& h : mock_ptr->last_headers) {
    if (h.find("Authorization: Bearer test-key") != std::string::npos) has_auth = true;
  }
  REQUIRE(has_auth);
}

TEST_CASE("OpenAIBackend full streaming callback", "[openai][streaming]") {
  OpenAIBackend::Config config;
  config.model = "gpt-4o-mini";
  config.host = "https://api.openai.com";
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

  OpenAIBackend backend(std::move(config), std::move(mock_http));

  std::vector<pu::backend::Message> history = {
    {pu::backend::Message::Role::kUser, "Hi"}
  };

  std::string accumulated;
  bool final_received = false;

  backend.Chat(history, [&](pu::backend::TokenType type,
                            std::string_view token,
                            bool is_final) {
    REQUIRE(type == pu::backend::TokenType::kContent);
    if (!token.empty()) accumulated += token;
    if (is_final) final_received = true;
  });

  REQUIRE(accumulated == "Hello world");
  REQUIRE(final_received == true);
  REQUIRE(mock_ptr->last_url == "https://api.openai.com/v1/chat/completions");
}
