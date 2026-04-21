// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "src/backends/ollama/ollama_backend.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace pu::backend;
using namespace pu::backends::ollama;
using namespace pu::tests;

TEST_CASE("OllamaBackend SSE parsing", "[ollama]") {
  SECTION("extracts content from valid line") {
    std::string line = R"({"message":{"content":"Hello"}})";
    auto token = OllamaBackend::ParseSseLine(line);
    REQUIRE(token.has_value());
    REQUIRE(token->content == "Hello");
    REQUIRE(token->done == false);
  }

  SECTION("recognizes done flag") {
    std::string line = R"({"done":true})";
    auto token = OllamaBackend::ParseSseLine(line);
    REQUIRE(token.has_value());
    REQUIRE(token->done == true);
  }

  SECTION("ignores heartbeat lines") {
    std::string line = R"({"other":"field"})";
    auto token = OllamaBackend::ParseSseLine(line);
    REQUIRE(!token.has_value());
  }

  SECTION("throws on invalid JSON") {
    std::string line = "not json";
    REQUIRE_THROWS_AS(OllamaBackend::ParseSseLine(line), std::runtime_error);
  }
}

TEST_CASE("OllamaBackend request building", "[ollama]") {
  Config config;
  config.model = "llama3.2:1b";
  config.temperature = 0.5f;
  config.system_prompt = "You are helpful.";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OllamaBackend backend(std::move(config), std::move(mock_http));

  std::vector<Message> history = {
    {Message::Role::kUser, "Hello"}
  };

  backend.Chat(history, [](TokenType, std::string_view, bool) {});

  // Verify request body contains expected fields
  auto body = nlohmann::json::parse(mock_ptr->last_body);
  REQUIRE(body["model"] == "llama3.2:1b");
  REQUIRE(body["stream"] == true);
  REQUIRE(body["options"]["temperature"] == 0.5f);
  REQUIRE(body["messages"].size() == 2);
  REQUIRE(body["messages"][0]["role"] == "system");
  REQUIRE(body["messages"][1]["role"] == "user");
}
