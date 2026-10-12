// SPDX-License-Identifier: GPL-3.0-only

#include "pu/config/backend.hpp"
#include "pu/llm/codebuddy.hpp"
#include "pu/llm/openai_provider.hpp"
#include "pu/llm/projection.hpp"
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
  const std::vector<std::string> headers = llm::CodeBuddyHeaders(llm::CodeBuddyConversationId());

  for (const char* name :
       {"X-Requested-With", "X-Domain", "X-Product", "X-Agent-Intent", "X-IDE-Type", "X-IDE-Name",
        "X-IDE-Version", "User-Agent", "X-Conversation-ID", "X-Conversation-Request-ID",
        "X-Conversation-Message-ID", "X-Request-ID", "X-User-Id", "x-stainless-lang"}) {
    REQUIRE(HasHeader(headers, name));
  }
  REQUIRE(HeaderValue(headers, "X-Domain") == "www.codebuddy.ai");
  REQUIRE(HeaderValue(headers, "X-Product") == "SaaS");
}

TEST_CASE("The conversation id is stable and the request ids are not", "[codebuddy]") {
  const std::string conversation_id = llm::CodeBuddyConversationId();
  const std::vector<std::string> first = llm::CodeBuddyHeaders(conversation_id);
  const std::vector<std::string> second = llm::CodeBuddyHeaders(conversation_id);

  REQUIRE(HeaderValue(first, "X-Conversation-ID") == conversation_id);
  REQUIRE(HeaderValue(second, "X-Conversation-ID") == conversation_id);
  REQUIRE(HeaderValue(first, "X-Conversation-ID").size() == 36);
  REQUIRE(HeaderValue(second, "X-Conversation-ID").size() == 36);

  REQUIRE(HeaderValue(first, "X-Request-ID") != HeaderValue(second, "X-Request-ID"));
  REQUIRE(HeaderValue(first, "X-Conversation-Request-ID") !=
          HeaderValue(second, "X-Conversation-Request-ID"));
  REQUIRE(HeaderValue(first, "X-Conversation-Message-ID") !=
          HeaderValue(second, "X-Conversation-Message-ID"));
  REQUIRE(HeaderValue(first, "X-Request-ID").size() == 32);

  REQUIRE(llm::CodeBuddyConversationId() != conversation_id);
}

TEST_CASE("A codebuddy backend is the OpenAI protocol at the CodeBuddy host", "[codebuddy]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kCodeBuddy;
  cfg.host = config::DefaultHostFor(config::BackendType::kCodeBuddy);
  cfg.model = "deepseek-v4-flash";
  cfg.api_key = "key";

  auto [mock_http, mock_ptr] = MakeMockHttpClient();
  auto provider = config::CreateBackend(cfg, std::move(mock_http));
  REQUIRE(provider != nullptr);

  std::vector<ChatMessage> history = {{1, "now", "user", "Hello"}};
  provider->Chat(ChatRequest(history, {}));

  REQUIRE(mock_ptr->last_url ==
          config::DefaultHostFor(config::BackendType::kCodeBuddy) + "/chat/completions");
  REQUIRE(mock_ptr->last_body.find("\"stream\":true") != std::string::npos);
  REQUIRE(HeaderValue(mock_ptr->last_headers, "Authorization") == "Bearer key");
  REQUIRE(HasHeader(mock_ptr->last_headers, "X-Agent-Intent"));
  REQUIRE(HasHeader(mock_ptr->last_headers, "X-Conversation-ID"));
}

TEST_CASE("A codebuddy provider keeps one conversation across its turns", "[codebuddy]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kCodeBuddy;
  cfg.host = config::DefaultHostFor(config::BackendType::kCodeBuddy);
  cfg.model = "deepseek-v4-flash";
  cfg.api_key = "key";

  auto [mock_http, mock_ptr] = MakeMockHttpClient();
  auto provider = config::CreateBackend(cfg, std::move(mock_http));
  REQUIRE(provider != nullptr);

  std::vector<ChatMessage> history = {{1, "now", "user", "Hello"}};
  provider->Chat(ChatRequest(history, {}));
  const std::string first_conversation = HeaderValue(mock_ptr->last_headers, "X-Conversation-ID");
  const std::string first_request = HeaderValue(mock_ptr->last_headers, "X-Request-ID");

  provider->Chat(ChatRequest(history, {}));

  REQUIRE(HeaderValue(mock_ptr->last_headers, "X-Conversation-ID") == first_conversation);
  REQUIRE(HeaderValue(mock_ptr->last_headers, "X-Request-ID") != first_request);
}

TEST_CASE("Two codebuddy providers are two conversations", "[codebuddy]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kCodeBuddy;
  cfg.host = config::DefaultHostFor(config::BackendType::kCodeBuddy);
  cfg.model = "deepseek-v4-flash";
  cfg.api_key = "key";

  auto [first_http, first_ptr] = MakeMockHttpClient();
  auto first = config::CreateBackend(cfg, std::move(first_http));
  auto [second_http, second_ptr] = MakeMockHttpClient();
  auto second = config::CreateBackend(cfg, std::move(second_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hello"}};
  first->Chat(ChatRequest(history, {}));
  second->Chat(ChatRequest(history, {}));

  REQUIRE(HeaderValue(first_ptr->last_headers, "X-Conversation-ID") !=
          HeaderValue(second_ptr->last_headers, "X-Conversation-ID"));
}

TEST_CASE("Every backend type carries a thinking level", "[codebuddy][openai][ollama]") {
  // Ollama takes one as a top-level `think`, the OpenAI-compatible types as
  // `reasoning_effort` / `extra_body`; the level is no longer a per-type carve-out.
  REQUIRE(config::DefaultHostFor(config::BackendType::kOllama) == "http://localhost:11434");
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
  provider->Chat(ChatRequest(history, {}));

  REQUIRE(mock_ptr->last_url == "http://127.0.0.1:9999/v1/chat/completions");
  REQUIRE(HasHeader(mock_ptr->last_headers, "X-Domain"));
}

TEST_CASE("CreateBackend refuses a backend that names no host", "[codebuddy]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kCodeBuddy;
  cfg.model = "deepseek-v4-flash";

  auto mock_http = std::make_shared<MockHttpClient>();
  REQUIRE_THROWS_AS(config::CreateBackend(cfg, std::move(mock_http)), pu::Error);
}
