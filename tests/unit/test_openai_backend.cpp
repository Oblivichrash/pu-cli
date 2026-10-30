// SPDX-License-Identifier: GPL-3.0-only

#include "pu/llm/openai_provider.hpp"
#include "pu/llm/projection.hpp"
#include "tests/mocks/provider_harness.hpp"
#include "pu/core/base.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/json.hpp>
#include "pu/core/json.hpp"

using namespace pu;
using namespace pu::tests;

namespace {

using Harness = ProviderHarness<OpenAIProvider, OpenAIProvider::Config>;

// The config every test here starts from: one model behind one gateway, with a key
// to send. A test that needs something else copies this and changes that one field.
OpenAIProvider::Config RemoteConfig(const char* model = "gpt-4o-mini",
                                    const char* host = "https://api.openai.com/v1") {
  OpenAIProvider::Config config;
  config.model = model;
  config.host = host;
  config.api_key = "test-key";
  return config;
}

}  // namespace

TEST_CASE("OpenAIProvider request building", "[openai]") {
  auto config = RemoteConfig();
  config.temperature = 0.7f;

  Harness harness(std::move(config));
  const auto body = harness.BodyOf("Hello");

  REQUIRE(body.at("model") == "gpt-4o-mini");
  REQUIRE(body.at("stream") == true);
  REQUIRE(body.at("temperature") == 0.7f);
  REQUIRE_FALSE(body.as_object().contains("max_tokens"));

  bool has_auth = false;
  for (const auto& h : harness.Mock().last_headers) {
    if (h.find("Authorization: Bearer test-key") != std::string::npos) has_auth = true;
  }
  REQUIRE(has_auth);
}

TEST_CASE("OpenAIProvider sends the token cap only when one is configured", "[openai]") {
  const auto body_for = [](std::optional<int> cap) {
    auto config = RemoteConfig();
    config.max_tokens = cap;
    Harness harness(std::move(config));
    return harness.BodyOf("Hello");
  };

  REQUIRE_FALSE(body_for(std::nullopt).as_object().contains("max_tokens"));
  REQUIRE(body_for(4096).at("max_tokens") == 4096);
}

TEST_CASE("OpenAIProvider does not send Authorization header when api_key is empty", "[openai]") {
  auto config = RemoteConfig("local-model", "http://localhost:8080/v1");
  config.api_key = "";

  Harness harness(std::move(config));
  harness->Chat(ChatRequest(harness.History("Hi"), {}));

  bool has_auth = false;
  for (const auto& h : harness.Mock().last_headers) {
    if (h.find("Authorization:") != std::string::npos) has_auth = true;
  }
  REQUIRE_FALSE(has_auth);
}

TEST_CASE("OpenAIProvider sends the extra headers a gateway asks for", "[openai]") {
  auto config = RemoteConfig("local-model", "http://localhost:8080/v1");
  config.extra_headers = [] { return std::vector<std::string>{"X-Domain: example.invalid"}; };

  Harness harness(std::move(config));
  harness->Chat(ChatRequest(harness.History("Hi"), {}));

  bool has_domain = false;
  for (const auto& h : harness.Mock().last_headers) {
    if (h == "X-Domain: example.invalid") has_domain = true;
  }
  REQUIRE(has_domain);
}

TEST_CASE("OpenAIProvider full streaming callback", "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");
  config.api_key = "test-key";

  Harness harness(std::move(config));

  std::vector<std::string> chunks = {R"(data: {"choices":[{"delta":{"content":"Hello"}}]})",
                                     R"(data: {"choices":[{"delta":{"content":" world"}}]})",
                                     R"(data: [DONE])"};

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    for (const auto& chunk : chunks) {
      std::string data = chunk + "\n";
      cb(data.data(), data.size());
    }
  };

  const std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};

  std::string accumulated;
  auto result = harness->Chat(ChatRequest(history, {}, [&](std::string_view token) { accumulated += token; }));

  REQUIRE(result.content == "Hello world");
  REQUIRE_FALSE(result.usage.has_value());
}

TEST_CASE("OpenAIProvider asks for token usage and reports it", "[openai][usage]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk = R"(data: {"choices":[{"delta":{"content":"hi"}}]})"
                        "\n"
                        R"(data: {"choices":[],"usage":{"prompt_tokens":11,"completion_tokens":7}})"
                        "\n"
                        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  auto body = harness.Body();
  REQUIRE(body.at("stream_options").at("include_usage") == true);

  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 11);
  REQUIRE(result.usage->completion_tokens == 7);
}

TEST_CASE("OpenAIProvider reads the counts it models from a real gateway response",
          "[openai][usage][fixture]") {
  auto config = RemoteConfig("deepseek-v4-flash");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string chunk = ReadFixture("codebuddy-usage.sse");
    cb(const_cast<char*>(chunk.data()), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "pong");
  REQUIRE(result.finish_reason == "stop");
  REQUIRE(result.model == "deepseek-v4-flash");
  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 549556);
  REQUIRE(result.usage->completion_tokens == 2);
  REQUIRE(result.usage->total_tokens == 549558);
  REQUIRE(result.usage->cached_tokens == 549376);

  // The frame carried more than the counts above, and the parts nothing reads yet are
  // kept as the gateway wrote them rather than dropped with the frame.
  REQUIRE(json::HasKey(result.raw_usage, "completion_tokens_details"));
  REQUIRE(json::HasKey(result.raw_usage, "prompt_tokens_details"));
  REQUIRE(result.raw_usage.at("prompt_cache_miss_tokens") == 180);
}

TEST_CASE("OpenAIProvider names the fields on a captured frame that nothing reads",
          "[openai][usage][fixture]") {
  auto config = RemoteConfig("deepseek-chat");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string chunk = ReadFixture("deepseek-chat.sse");
    cb(const_cast<char*>(chunk.data()), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  // The set is asserted whole rather than one name at a time: a gateway that adds a field
  // to what this capture holds fails here, which is the signal that a name the backend
  // should have read may have arrived and been missed by every read above.
  REQUIRE(result.unread_fields ==
          std::vector<std::string>{"id", "object", "created", "system_fingerprint"});
}

TEST_CASE("OpenAIProvider handles HTTP errors", "[openai][error]") {
  auto config = RemoteConfig("gpt-4o-mini");
  config.api_key = "invalid-key";

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback) {
    throw pu::HttpError("HTTP error 401: Unauthorized");
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  REQUIRE_THROWS_AS(harness->Chat(ChatRequest(history, {})), pu::HttpError);
}

TEST_CASE("OpenAIProvider tool calling stream", "[openai][tools]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk1 =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"exec","arguments":"ls"}}]}}]})" +
        std::string("\n");
    std::string chunk2 = "data: [DONE]\n";
    cb(chunk1.data(), chunk1.size());
    cb(chunk2.data(), chunk2.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "list"}};
  ToolDefinition tool;
  tool.name = "exec";
  tool.parameters = boost::json::object{};
  std::vector<ToolDefinition> tools = {tool};

  auto result = harness->Chat(ChatRequest(history, tools, [](std::string_view) {}));

  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].id == "call_1");
  REQUIRE(result.tool_calls[0].name == "exec");
}

TEST_CASE("OpenAIProvider disables thinking with the marker the none level sends", "[openai]") {
  auto config = RemoteConfig("deepseek-reasoner", "https://api.deepseek.com/v1");
  config.api_key = "test-key";
  config.thinking = ThinkingLevel::kNone;

  Harness harness(std::move(config));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  harness->Chat(ChatRequest(history, {}));

  auto body = harness.Body();
  REQUIRE(json::HasKey(body, "extra_body"));
  REQUIRE(body.at("extra_body").at("thinking").at("type") == "disabled");
}

TEST_CASE("OpenAIProvider sends nothing about thinking for the absent level", "[openai]") {
  auto config = RemoteConfig("deepseek-reasoner", "https://api.deepseek.com/v1");
  config.api_key = "test-key";
  config.thinking = ThinkingLevel::kServerDefault;

  Harness harness(std::move(config));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  harness->Chat(ChatRequest(history, {}));

  auto body = harness.Body();
  REQUIRE_FALSE(json::HasKey(body, "extra_body"));
  REQUIRE_FALSE(json::HasKey(body, "reasoning_effort"));
}

TEST_CASE("OpenAIProvider sends a named level as reasoning_effort", "[openai]") {
  for (const ThinkingLevel level :
       {ThinkingLevel::kLow, ThinkingLevel::kMedium, ThinkingLevel::kHigh}) {
    auto config = RemoteConfig("o4-mini");
    config.api_key = "test-key";
    config.thinking = level;

    Harness harness(std::move(config));

    const std::vector<ChatMessage> history = {{1, "now", "user", "think hard"}};
    harness->Chat(ChatRequest(history, {}));

    auto body = harness.Body();
    REQUIRE(body.at("reasoning_effort") == ThinkingLevelName(level));
    REQUIRE_FALSE(json::HasKey(body, "extra_body"));
  }
}

TEST_CASE("OpenAIProvider reports why the reply stopped", "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk = R"(data: {"choices":[{"delta":{"content":"half"},"finish_reason":null}]})"
                        "\n"
                        R"(data: {"choices":[{"delta":{},"finish_reason":"length"}]})"
                        "\n"
                        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "write a lot"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "half");
  REQUIRE(result.finish_reason == "length");
}

TEST_CASE("OpenAIProvider raises an error sent inside the stream", "[openai][error]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"error":{"message":"rate limit reached","type":"rate_limit_error"}})"
        "\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  try {
    harness->Chat(ChatRequest(history, {}));
    FAIL("an error inside the stream should reach the caller");
  } catch (const std::exception& e) {
    REQUIRE(std::string(e.what()).find("rate limit reached") != std::string::npos);
  }
}

TEST_CASE("OpenAIProvider keeps a refusal as the reply", "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk = R"(data: {"choices":[{"delta":{"refusal":"I cannot help with that"}}]})"
                        "\n"
                        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "I cannot help with that");
}

TEST_CASE("OpenAIProvider keeps a refusal that arrives beside content", "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"content":"Here is the answer"}}]})"
        "\n"
        R"(data: {"choices":[{"delta":{"refusal":"I cannot help with that"}}]})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "Here is the answer\nI cannot help with that");
}

TEST_CASE("OpenAIProvider keeps tool calls from a stream that ends without its sentinel",
          "[openai][tools]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"exec","arguments":"{}"}}]}}]})" +
        std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "list"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].name == "exec");
}

TEST_CASE("OpenAIProvider rejects a tool call the token cap cut short", "[openai][tools]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  // The stream ends on the token cap with the arguments half written, which is what
  // a reply that runs out of budget looks like: finish_reason "length", not a full
  // JSON object.
  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"write_file","arguments":"{\"path\": \"/tmp/a\","}}]},"finish_reason":"length"}]})" +
        std::string("\n");
    std::string done = "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
    cb(done.data(), done.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "write"}};
  REQUIRE_THROWS_AS(harness->Chat(ChatRequest(history, {})), pu::GenericError);
}

TEST_CASE("OpenAIProvider keeps a tool call whose arguments are not JSON", "[openai][tools]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  // A complete, non-JSON argument string is a backend that answers with something
  // other than JSON; it is not the token cap cutting a reply short.
  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"exec","arguments":"ls -la"}}]}}]})" +
        std::string("\n");
    std::string done = "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
    cb(done.data(), done.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "run"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].name == "exec");
}

TEST_CASE("OpenAIProvider keeps a tool call that arrives without an index", "[openai][tools]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"tool_calls":[{"id":"call_7","function":{"name":"exec","arguments":"{}"}}]}}]})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "list"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].id == "call_7");
  REQUIRE(result.tool_calls[0].arguments.is_object());
}

TEST_CASE("OpenAIProvider reads a frame that carries message instead of delta",
          "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"message":{"content":"the whole answer"},"finish_reason":"stop"}]})" +
        std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "the whole answer");
  REQUIRE(result.finish_reason == "stop");
}

TEST_CASE("OpenAIProvider reports the model that answered", "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");
  config.model = "gpt-4o-mini";  // what was asked for

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"model":"gpt-4o-mini-2024-07-18","choices":[{"delta":{"content":"hi"},"finish_reason":null}]})"
        "\n"
        R"(data: {"model":"gpt-4o-mini-2024-07-18","choices":[{"delta":{},"finish_reason":"stop"}]})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.model == "gpt-4o-mini-2024-07-18");
}

TEST_CASE("OpenAIProvider hands reasoning to the caller as it arrives", "[openai][streaming]") {
  auto config = RemoteConfig("deepseek-reasoner");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"reasoning_content":"weighing "}}]})"
        "\n"
        R"(data: {"choices":[{"delta":{"reasoning_content":"the options"}}]})"
        "\n"
        R"(data: {"choices":[{"delta":{"content":"answer"},"finish_reason":"stop"}]})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "think"}};
  std::string streamed;
  std::string content;
  ChatRequest request(history, {});
  request.on_content = [&](std::string_view token) { content += token; };
  request.on_reasoning = [&](std::string_view token) { streamed += token; };
  auto result = harness->Chat(request);

  REQUIRE(streamed == "weighing the options");
  REQUIRE(content == "answer");
  REQUIRE(llm::ProjectReasoningText(result.reasoning) == "weighing the options");
}

TEST_CASE("OpenAIProvider assembles one payload split over several data lines",
          "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        "data: {\"choices\":[{\"delta\":{\"content\":\n"
        "data: \"split answer\"}}]}\n"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "split answer");
}

TEST_CASE("OpenAIProvider reads a data field with no space", "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        "data:{\"choices\":[{\"delta\":{\"content\":\"bare\"}}]}\n"
        "data:[DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "bare");
}

TEST_CASE("OpenAIProvider ignores the fields that name an event", "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        ": keep-alive\n"
        "event: message\n"
        "id: 7\n"
        "retry: 100\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "ok");
}

TEST_CASE("OpenAIProvider reads the counts a cached prompt is billed with", "[openai][usage]") {
  auto config = RemoteConfig("gpt-4o-mini");
  // What the product says a call costs, and where: the provider is told by the gateway that
  // built it, because the wire does not say whether a price is there or what it is in.
  config.billing = {pu::config::BillingKind::kReported, "credit", "credits"};

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"delta":{"content":"hi"}}]})"
        "\n"
        R"(data: {"choices":[],"usage":{"prompt_tokens":12,"completion_tokens":3,"total_tokens":15,"prompt_tokens_details":{"cached_tokens":9},"completion_tokens_details":{"reasoning_tokens":1},"credit":0.05}})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->total_tokens == 15);
  REQUIRE(result.usage->cached_tokens == 9);
  REQUIRE(result.usage->cost.has_value());
  REQUIRE(result.usage->cost->amount == 0.05);
  // The unit comes from the product the file names, not from the wire: the number alone does
  // not say what it is denominated in.
  REQUIRE(result.usage->cost->unit == "credits");
  REQUIRE(json::HasKey(result.raw_usage, "completion_tokens_details"));
}

TEST_CASE("OpenAIProvider reads a cached prompt the gateway names at the top level",
          "[openai][usage]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[],"usage":{"prompt_tokens":100,"completion_tokens":1,"prompt_cache_hit_tokens":96,"prompt_cache_miss_tokens":4}})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->cached_tokens == 96);
  REQUIRE(result.raw_usage.at("prompt_cache_miss_tokens") == 4);
}

TEST_CASE("A shape renames the fields the reply is read from", "[openai][usage]") {
  auto config = RemoteConfig("nim-model");
  config.shape.reasoning_field = "reasoning";
  config.shape.content_field = "text";
  config.shape.usage_field = "token_usage";
  config.shape.prompt_tokens_field = "input_tokens";
  config.shape.completion_tokens_field = "output_tokens";
  config.shape.cached_tokens_parent = "";
  config.shape.cached_tokens_top_level_field = "cached_tokens";

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"index":0,"delta":{"text":"hi","reasoning":"why"}}]})"
        "\n"
        R"(data: {"choices":[{"index":0,"finish_reason":"stop"}],"token_usage":{"input_tokens":7,"output_tokens":2,"cached_tokens":5}})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "hi");
  REQUIRE(result.reasoning.size() == 1);
  REQUIRE(result.reasoning[0].text == "why");
  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 7);
  REQUIRE(result.usage->completion_tokens == 2);
  REQUIRE(result.usage->cached_tokens == 5);
}

TEST_CASE("The default shape reads the OpenAI field names", "[openai][usage]") {
  const llm::ResponseShape shape;
  REQUIRE(shape.content_field == "content");
  REQUIRE(shape.reasoning_field == "reasoning_content");
  REQUIRE(shape.usage_field == "usage");
  REQUIRE(shape.prompt_tokens_field == "prompt_tokens");
  REQUIRE(shape.cached_tokens_top_level_field == "prompt_cache_hit_tokens");
  REQUIRE(shape.finish_reason_field == "finish_reason");
}

TEST_CASE("The default shape reads a captured DeepSeek stream", "[openai][usage]") {
  auto config = RemoteConfig("deepseek-chat");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string chunk = ReadFixture("deepseek-chat.sse");
    cb(const_cast<char*>(chunk.data()), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "pong");
  REQUIRE(result.finish_reason == "stop");
  REQUIRE(result.model == "deepseek-flash");
  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 11);
  REQUIRE(result.usage->completion_tokens == 2);
  REQUIRE(result.usage->total_tokens == 13);
  REQUIRE(result.usage->cached_tokens == 0);
}

TEST_CASE("OpenAIProvider reads the reply from one completion only", "[openai][streaming]") {
  auto config = RemoteConfig("gpt-4o-mini");

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"(data: {"choices":[{"index":0,"delta":{"content":"asked"},"finish_reason":"stop"},{"index":1,"delta":{"content":"unasked"},"finish_reason":"stop"}]})"
        "\n"
        "data: [DONE]\n";
    cb(chunk.data(), chunk.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "asked");
  REQUIRE(result.finish_reason == "stop");
}

TEST_CASE("[openai] a reply that never carries finish_reason is reported, not assumed complete",
          "[openai]") {
  // A renamed field would leave a truncated reply looking finished.
  auto config = RemoteConfig("m");
  config.api_key = "k";

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [](const std::string&, const std::string&,
                                   const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string frame =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"},\"stop_reason\":\"length\"}]}\n\n"
        "data: [DONE]\n\n";
    cb(const_cast<char*>(frame.data()), frame.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  const ChatResult result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.content == "hi");
  REQUIRE(result.finish_reason.empty());
  REQUIRE(std::find(result.missing_fields.begin(), result.missing_fields.end(),
                    std::string{"finish_reason"}) != result.missing_fields.end());
}

TEST_CASE("[openai] a reply carrying every field the shape reads reports nothing missing",
          "[openai]") {
  auto config = RemoteConfig("m");
  config.api_key = "k";

  Harness harness(std::move(config));

  harness.Mock().simulate_response = [](const std::string&, const std::string&,
                                   const std::vector<std::string>&, pu::http::WriteCallback cb) {
    const std::string frame =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2,"
        "\"prompt_tokens_details\":{\"cached_tokens\":0}}}\n\n"
        "data: [DONE]\n\n";
    cb(const_cast<char*>(frame.data()), frame.size());
  };

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  const ChatResult result = harness->Chat(ChatRequest(history, {}));

  REQUIRE(result.missing_fields.empty());
}
