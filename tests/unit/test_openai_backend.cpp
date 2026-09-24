// SPDX-License-Identifier: GPL-3.0-only

#include "pu/llm/openai_provider.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/core/base.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/json.hpp>
#include "pu/core/json.hpp"

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

  std::vector<ChatMessage> history = {ChatMessage{1, "now", "user", "Hello"}};

  provider.Chat(history, {});

  auto body = boost::json::parse(mock_ptr->last_body);
  REQUIRE(body.at("model") == "gpt-4o-mini");
  REQUIRE(body.at("stream") == true);
  REQUIRE(body.at("temperature") == 0.7f);

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

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
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

  std::vector<std::string> chunks = {R"(data: {"choices":[{"delta":{"content":"Hello"}}]})",
                                     R"(data: {"choices":[{"delta":{"content":" world"}}]})",
                                     R"(data: [DONE])"};

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    for (const auto& chunk : chunks) {
      std::string data = chunk + "\n";
      cb(data.data(), data.size());
    }
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {ChatMessage{1, "now", "user", "Hi"}};

  std::string accumulated;
  auto result = provider.Chat(history, {}, [&](const std::string& token) { accumulated += token; });

  REQUIRE(result.content == "Hello world");
  // Nothing counted the tokens, so the counts stay absent rather than zero.
  REQUIRE_FALSE(result.usage.has_value());
}

TEST_CASE("OpenAIProvider asks for token usage and reports it", "[openai][usage]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";
  config.host = "https://api.openai.com/v1";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk = R"(data: {"choices":[{"delta":{"content":"hi"}}]})"
                        "\n"
                        R"(data: {"choices":[],"usage":{"prompt_tokens":11,"completion_tokens":7}})"
                        "\n"
                        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = provider.Chat(history, {});

  // The stream carries no usage unless the request asks for it.
  auto body = boost::json::parse(mock_ptr->last_body);
  REQUIRE(body.at("stream_options").at("include_usage") == true);

  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 11);
  REQUIRE(result.usage->completion_tokens == 7);
}

TEST_CASE("OpenAIProvider handles HTTP errors", "[openai][error]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";
  config.api_key = "invalid-key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback) {
    throw pu::HttpError("HTTP error 401: Unauthorized");
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  REQUIRE_THROWS_AS(provider.Chat(history, {}), pu::HttpError);
}

TEST_CASE("OpenAIProvider tool calling stream", "[openai][tools]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";
  config.host = "https://api.openai.com/v1";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk1 =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"exec","arguments":"ls"}}]}}]})" +
        std::string("\n");
    std::string chunk2 = "data: [DONE]\n";
    cb(chunk1.data(), chunk1.size());
    cb(chunk2.data(), chunk2.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "list"}};
  ToolDefinition tool;
  tool.name = "exec";
  tool.parameters = boost::json::object{};
  std::vector<ToolDefinition> tools = {tool};

  auto result = provider.Chat(history, tools, [](const std::string&) {});

  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].id == "call_1");
  REQUIRE(result.tool_calls[0].name == "exec");
}

TEST_CASE("OpenAIProvider adds extra_body to disable thinking when enable_thinking=false",
          "[openai]") {
  OpenAIProvider::Config config;
  config.model = "deepseek-reasoner";
  config.host = "https://api.deepseek.com/v1";
  config.api_key = "test-key";
  config.enable_thinking = false;

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  provider.Chat(history, {});

  auto body = boost::json::parse(mock_ptr->last_body);
  REQUIRE(json::HasKey(body, "extra_body"));
  REQUIRE(body.at("extra_body").at("thinking").at("type") == "disabled");
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

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  provider.Chat(history, {});

  auto body = boost::json::parse(mock_ptr->last_body);
  REQUIRE_FALSE(json::HasKey(body, "extra_body"));
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

TEST_CASE("OpenAIProvider reports why the reply stopped", "[openai][streaming]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk = R"(data: {"choices":[{"delta":{"content":"half"},"finish_reason":null}]})"
                        "\n"
                        R"(data: {"choices":[{"delta":{},"finish_reason":"length"}]})"
                        "\n"
                        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "write a lot"}};
  auto result = provider.Chat(history, {});

  // Null while the answer is coming, named once the provider is done with it.
  REQUIRE(result.content == "half");
  REQUIRE(result.finish_reason == "length");
}

TEST_CASE("OpenAIProvider raises an error sent inside the stream", "[openai][error]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"error":{"message":"rate limit reached","type":"rate_limit_error"}})"
        "\n";
    cb(chunk.data(), chunk.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  try {
    provider.Chat(history, {});
    FAIL("an error inside the stream should reach the caller");
  } catch (const std::exception& e) {
    // The provider's own words, not a generic empty-answer diagnosis.
    REQUIRE(std::string(e.what()).find("rate limit reached") != std::string::npos);
  }
}

TEST_CASE("OpenAIProvider keeps a refusal as the reply", "[openai][streaming]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk = R"(data: {"choices":[{"delta":{"refusal":"I cannot help with that"}}]})"
                        "\n"
                        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = provider.Chat(history, {});

  // Without this the refusal is an empty answer with no reason attached.
  REQUIRE(result.content == "I cannot help with that");
}

TEST_CASE("OpenAIProvider keeps tool calls from a stream that ends without its sentinel",
          "[openai][tools]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"exec","arguments":"{}"}}]}}]})" +
        std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "list"}};
  auto result = provider.Chat(history, {});

  // The fragments are calls the provider already made, sentinel or not.
  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].name == "exec");
}

TEST_CASE("OpenAIProvider keeps a tool call that arrives without an index", "[openai][tools]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"id":"call_7","function":{"name":"exec","arguments":"{}"}}]}}]})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "list"}};
  auto result = provider.Chat(history, {});

  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].id == "call_7");
  REQUIRE(result.tool_calls[0].arguments.is_object());
}

TEST_CASE("OpenAIProvider reads a frame that carries message instead of delta",
          "[openai][streaming]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"message":{"content":"the whole answer"},"finish_reason":"stop"}]})" +
        std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = provider.Chat(history, {});

  // A gateway that answers in one frame is read the same way a streaming one is.
  REQUIRE(result.content == "the whole answer");
  REQUIRE(result.finish_reason == "stop");
}

TEST_CASE("OpenAIProvider reports the model that answered", "[openai][streaming]") {
  OpenAIProvider::Config config;
  config.model = "gpt-4o-mini";  // what was asked for

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"model":"gpt-4o-mini-2024-07-18","choices":[{"delta":{"content":"hi"},"finish_reason":null}]})"
        "\n"
        R"(data: {"model":"gpt-4o-mini-2024-07-18","choices":[{"delta":{},"finish_reason":"stop"}]})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  OpenAIProvider provider(config, std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = provider.Chat(history, {});

  // The dated build the provider served, not the tag that was requested.
  REQUIRE(result.model == "gpt-4o-mini-2024-07-18");
}
