// SPDX-License-Identifier: GPL-3.0-only

#include "pu/config/backend.hpp"
#include "pu/llm/codebuddy.hpp"
#include "pu/llm/openai_provider.hpp"
#include "tests/mocks/mock_http_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace pu;
using namespace pu::tests;

namespace {

bool HasHeader(const std::vector<std::string>& headers, const std::string& name) {
  const std::string prefix = name + ":";
  for (const std::string& header : headers) {
    if (header.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

std::string HeaderValue(const std::vector<std::string>& headers, const std::string& name) {
  const std::string prefix = name + ": ";
  for (const std::string& header : headers) {
    if (header.rfind(prefix, 0) == 0) return header.substr(prefix.size());
  }
  return "";
}

}  // namespace

TEST_CASE("The CodeBuddy header set is the one the gateway is called by", "[codebuddy]") {
  const std::vector<std::string> headers = llm::CodeBuddyHeaders();

  for (const char* name :
       {"X-Requested-With", "X-Domain", "X-Product", "X-Agent-Intent", "X-IDE-Type", "X-IDE-Name",
        "X-IDE-Version", "User-Agent", "X-Conversation-ID", "X-Conversation-Request-ID",
        "X-Conversation-Message-ID", "X-Request-ID", "X-User-Id", "x-stainless-lang"}) {
    REQUIRE(HasHeader(headers, name));
  }
  REQUIRE(HeaderValue(headers, "X-Domain") == "www.codebuddy.ai");
  REQUIRE(HeaderValue(headers, "X-Product") == "SaaS");
}

TEST_CASE("The CodeBuddy correlation ids are generated per call", "[codebuddy]") {
  const std::vector<std::string> first = llm::CodeBuddyHeaders();
  const std::vector<std::string> second = llm::CodeBuddyHeaders();

  REQUIRE(HeaderValue(first, "X-Request-ID") != HeaderValue(second, "X-Request-ID"));
  REQUIRE(HeaderValue(first, "X-Conversation-ID") != HeaderValue(second, "X-Conversation-ID"));
  REQUIRE(HeaderValue(first, "X-Request-ID").size() == 32);
  REQUIRE(HeaderValue(first, "X-Conversation-ID").size() == 36);
}

TEST_CASE("A codebuddy backend is the OpenAI protocol at the CodeBuddy host", "[codebuddy]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kCodeBuddy;
  cfg.host = llm::kCodeBuddyHost;
  cfg.model = "deepseek-v4-flash";
  cfg.api_key = "key";

  auto [mock_http, mock_ptr] = MakeMockHttpClient();
  auto provider = config::CreateBackend(cfg, std::move(mock_http));
  REQUIRE(provider != nullptr);

  std::vector<ChatMessage> history = {{1, "now", "user", "Hello"}};
  provider->Chat(history, {});

  REQUIRE(mock_ptr->last_url == std::string(llm::kCodeBuddyHost) + "/chat/completions");
  REQUIRE(mock_ptr->last_body.find("\"stream\":true") != std::string::npos);
  REQUIRE(HeaderValue(mock_ptr->last_headers, "Authorization") == "Bearer key");
  REQUIRE(HasHeader(mock_ptr->last_headers, "X-Agent-Intent"));
  REQUIRE(HasHeader(mock_ptr->last_headers, "X-Conversation-ID"));
}

TEST_CASE("Thinking level capability follows the backend type", "[codebuddy][openai][ollama]") {
  REQUIRE(config::CarriesThinkingLevel(config::BackendType::kCodeBuddy));
  REQUIRE(config::CarriesThinkingLevel(config::BackendType::kOpenAI));
  REQUIRE_FALSE(config::CarriesThinkingLevel(config::BackendType::kOllama));
}

TEST_CASE("A configured host is not overridden", "[codebuddy]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kCodeBuddy;
  cfg.host = "http://127.0.0.1:9999/v1";
  cfg.model = "deepseek-v4-flash";

  auto [mock_http, mock_ptr] = MakeMockHttpClient();
  auto provider = config::CreateBackend(cfg, std::move(mock_http));
  REQUIRE(provider != nullptr);

  std::vector<ChatMessage> history = {{1, "now", "user", "Hello"}};
  provider->Chat(history, {});

  REQUIRE(mock_ptr->last_url == "http://127.0.0.1:9999/v1/chat/completions");
  REQUIRE(HasHeader(mock_ptr->last_headers, "X-Domain"));
}

TEST_CASE("CreateBackend refuses a backend that names no host", "[codebuddy]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kCodeBuddy;
  cfg.model = "deepseek-v4-flash";

  auto mock_http = std::make_unique<MockHttpClient>();
  REQUIRE_THROWS_AS(config::CreateBackend(cfg, std::move(mock_http)), pu::Error);
}
