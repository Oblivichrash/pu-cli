// SPDX-License-Identifier: GPL-3.0-only

#include "pu/llm/ollama_provider.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include <catch2/catch_test_macros.hpp>
#include <boost/json.hpp>

using namespace pu;
using namespace pu::tests;

TEST_CASE("OllamaProvider request building", "[ollama]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.temperature = 0.5f;
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {ChatMessage{1, "now", "user", "Hello"}};

  provider.Chat(history, {});

  auto body = boost::json::parse(mock_ptr->last_body);
  REQUIRE(body.at("model") == "llama3.2:1b");
  REQUIRE(body.at("stream") == true);
  REQUIRE(body.at("options").at("temperature") == 0.5f);
}

TEST_CASE("OllamaProvider full streaming callback", "[ollama][streaming]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  std::vector<std::string> chunks = {R"({"message":{"content":"Hello"}})",
                                     R"({"message":{"content":" world"}})", R"({"done":true})"};

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    for (const auto& chunk : chunks) {
      std::string data = chunk + "\n";
      cb(data.data(), data.size());
    }
  };

  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {ChatMessage{1, "now", "user", "Hi"}};

  std::string accumulated;
  bool final_received = false;

  auto result = provider.Chat(history, {}, [&](const std::string& token) { accumulated += token; });

  REQUIRE(result.content == "Hello world");
  // Nothing counted the tokens, so the counts stay absent rather than zero.
  REQUIRE_FALSE(result.usage.has_value());
}

TEST_CASE("OllamaProvider reports the token counts it was sent", "[ollama][usage]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"({"message":{"content":"hi"}})"
        "\n"
        R"({"message":{"content":""},"done":true,"prompt_eval_count":21,"eval_count":9})"
        "\n";
    cb(chunk.data(), chunk.size());
  };

  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = provider.Chat(history, {});

  REQUIRE(result.usage.has_value());
  REQUIRE(result.usage->prompt_tokens == 21);
  REQUIRE(result.usage->completion_tokens == 9);
}

TEST_CASE("OllamaProvider tool calling stream", "[ollama][tools]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string data =
        R"({"message":{"content":"Running ls","tool_calls":[{"function":{"name":"execute_bash","arguments":{"command":"ls"}}}]}})" +
        std::string("\n");
    std::string done = R"({"done":true})" + std::string("\n");
    cb(data.data(), data.size());
    cb(done.data(), done.size());
  };

  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "list files"}};
  ToolDefinition tool;
  tool.name = "execute_bash";
  tool.parameters = boost::json::object{};
  std::vector<ToolDefinition> tools = {tool};

  auto result = provider.Chat(history, tools, [](const std::string&) {});

  REQUIRE(result.tool_calls.size() == 1);
  REQUIRE(result.tool_calls[0].name == "execute_bash");
}

TEST_CASE("OllamaProvider passes tool_call_id for tool messages", "[ollama][tools]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OllamaProvider provider(std::move(config), std::move(mock_http));

  ChatMessage tool_msg;
  tool_msg.id = 1;
  tool_msg.role = "tool";
  tool_msg.content = "result";
  tool_msg.tool_name = "execute_bash";
  tool_msg.tool_call_id = "call_42";
  std::vector<ChatMessage> history = {tool_msg};

  provider.Chat(history, {});

  auto body = boost::json::parse(mock_ptr->last_body);
  REQUIRE(body.at("messages").at(0).at("role") == "tool");
  REQUIRE(body.at("messages").at(0).at("tool_name") == "execute_bash");
  REQUIRE(body.at("messages").at(0).at("tool_call_id") == "call_42");
}

TEST_CASE("OllamaProvider keeps tool call names and arguments", "[ollama][tools]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OllamaProvider provider(std::move(config), std::move(mock_http));

  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","type":"function","function":{"name":"ls","arguments":{"path":"."}}}])");
  std::vector<ChatMessage> history = {assistant};

  ToolDefinition tool;
  tool.name = "ls";
  tool.parameters = boost::json::object{};
  provider.Chat(history, {tool});

  auto body = boost::json::parse(mock_ptr->last_body);
  auto& sent = body.at("messages").at(0).at("tool_calls").as_array()[0];
  REQUIRE(sent.at("id") == "call_1");
  REQUIRE(sent.at("function").at("name") == "ls");
  REQUIRE(sent.at("function").at("arguments").at("path") == ".");
}

TEST_CASE("OllamaProvider decodes arguments sent as a JSON string", "[ollama][tools]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();
  OllamaProvider provider(std::move(config), std::move(mock_http));

  ChatMessage assistant;
  assistant.role = "assistant";
  assistant.tool_calls = boost::json::parse(
      R"([{"id":"call_1","function":{"name":"ls","arguments":"{\"path\":\".\"}"}}])");
  std::vector<ChatMessage> history = {assistant};

  ToolDefinition tool;
  tool.name = "ls";
  tool.parameters = boost::json::object{};
  provider.Chat(history, {tool});

  auto body = boost::json::parse(mock_ptr->last_body);
  auto& sent = body.at("messages").at(0).at("tool_calls").as_array()[0];
  REQUIRE(sent.at("function").at("arguments").is_object());
  REQUIRE(sent.at("function").at("arguments").at("path") == ".");
}

TEST_CASE("OllamaProvider IsThinkingMode returns false", "[ollama]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  OllamaProvider provider(std::move(config), std::move(mock_http));
  REQUIRE(provider.IsThinkingMode() == false);
}

TEST_CASE("OllamaProvider reports why the reply stopped", "[ollama][streaming]") {
  OllamaProvider::Config config;
  config.model = "llama3.2:1b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"({"message":{"content":"half"},"done":true,"done_reason":"length"})" + std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "write a lot"}};
  auto result = provider.Chat(history, {});

  REQUIRE(result.content == "half");
  // Its own word for the token limit, under a name of its own.
  REQUIRE(result.finish_reason == "length");
}

TEST_CASE("OllamaProvider keeps the reasoning a thinking model reports", "[ollama][streaming]") {
  OllamaProvider::Config config;
  config.model = "deepseek-r1:7b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        R"({"message":{"content":"","thinking":"weighing the options"},"done":true})" +
        std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "think about it"}};
  auto result = provider.Chat(history, {});

  // Reasoning arrives under a name of its own, beside an empty content.
  REQUIRE(result.reasoning_content == "weighing the options");
  REQUIRE(result.content.empty());
}

TEST_CASE("OllamaProvider raises an error sent inside the stream", "[ollama][error]") {
  OllamaProvider::Config config;
  config.model = "missing-model";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk = R"({"error":"model not found"})" + std::string("\n");
    cb(chunk.data(), chunk.size());
  };

  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  try {
    provider.Chat(history, {});
    FAIL("an error inside the stream should reach the caller");
  } catch (const std::exception& e) {
    REQUIRE(std::string(e.what()).find("model not found") != std::string::npos);
  }
}

TEST_CASE("OllamaProvider reports the model that answered", "[ollama][streaming]") {
  OllamaProvider::Config config;
  config.model = "llama3.2";  // a tag, which the daemon resolves to a build
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk =
        std::string(R"({"model":"llama3.2:1b","message":{"content":"hi"},"done":false})") + "\n" +
        std::string(
            R"({"model":"llama3.2:1b","message":{"content":""},"done":true,"done_reason":"stop"})") +
        "\n";
    cb(chunk.data(), chunk.size());
  };

  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "Hi"}};
  auto result = provider.Chat(history, {});

  // The build that answered, not the tag that was requested.
  REQUIRE(result.model == "llama3.2:1b");
}

TEST_CASE("OllamaProvider hands reasoning to the caller as it arrives", "[ollama][streaming]") {
  OllamaProvider::Config config;
  config.model = "deepseek-r1:7b";
  config.host = "http://localhost:11434";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto* mock_ptr = mock_http.get();

  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::string chunk = R"({"message":{"content":"","thinking":"weighing "}})"
                        "\n"
                        R"({"message":{"content":"","thinking":"the options"}})"
                        "\n"
                        R"({"message":{"content":"answer"},"done":true,"done_reason":"stop"})"
                        "\n";
    cb(chunk.data(), chunk.size());
  };

  OllamaProvider provider(std::move(config), std::move(mock_http));

  std::vector<ChatMessage> history = {{1, "now", "user", "think"}};
  std::string streamed;
  std::string content;
  auto result = provider.Chat(
      history, {}, [&](const std::string& token) { content += token; }, nullptr,
      [&](const std::string& token) { streamed += token; });

  // The two channels arrive while the stream is open, and stay apart.
  REQUIRE(streamed == "weighing the options");
  REQUIRE(content == "answer");
  REQUIRE(result.reasoning_content == "weighing the options");
}
