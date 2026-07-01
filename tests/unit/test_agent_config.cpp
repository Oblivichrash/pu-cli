// SPDX-License-Identifier: GPL-3.0-only

#include "pu/agent_config.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/backend.hpp"
#include "pu/error_codes.hpp"
#include "pu/token_adapter.hpp"
#include "backends/ollama/ollama_token_adapter.hpp"
#include "backends/openai/openai_token_adapter.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <cstring>
#endif

using namespace pu::config;
using namespace pu::tests;
using namespace pu;

namespace fs = std::filesystem;

#ifdef _WIN32
static void set_env(const char* name, const char* value) {
    std::string s = std::string(name) + "=" + value;
    _putenv(s.c_str());
}
static void unset_env(const char* name) {
    std::string s = std::string(name) + "=";
    _putenv(s.c_str());
}
#else
static void set_env(const char* name, const char* value) {
    setenv(name, value, 1);
}
static void unset_env(const char* name) {
    unsetenv(name);
}
#endif

struct TempConfigFile {
  fs::path path;
  TempConfigFile() {
    path = fs::temp_directory_path() / "pu_test_agents.json";
  }
  ~TempConfigFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  void write(const std::string& content) {
    std::ofstream file(path);
    file << content;
  }
};

TEST_CASE("LoadAgentsConfig parses valid JSON", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "chat",
    "agents": [
      {
        "name": "chat",
        "type": "llm",
        "description": "Local Chat",
        "backend": {
          "type": "ollama",
          "host": "http://localhost:11434",
          "model": "qwen3.5:4b",
          "temperature": 0.7
        }
      },
      {
        "name": "bash",
        "type": "llm",
        "description": "Command Runner",
        "backend": {
          "type": "openai",
          "host": "https://api.openai.com/v1",
          "api_key": "${OPENAI_KEY}",
          "model": "gpt-4o-mini"
        },
        "security": {
          "sandbox_root": "/tmp"
        }
      }
    ]
  })";
  tmp.write(json);

  set_env("OPENAI_KEY", "test-key-123");

  std::error_code ec;
  AgentsConfig config = LoadAgentsConfig(tmp.path.string(), ec);
  REQUIRE_FALSE(ec);

  REQUIRE(config.default_agent == "chat");
  REQUIRE(config.agents.size() == 2);
  REQUIRE(config.agents[0].name == "chat");
  REQUIRE(config.agents[0].backend.type == BackendType::kOllama);
  REQUIRE(config.agents[1].name == "bash");
  REQUIRE(config.agents[1].backend.type == BackendType::kOpenAI);
  REQUIRE(config.agents[1].backend.api_key == "test-key-123");
  REQUIRE(config.agents[1].security.sandbox_root == "/tmp");

  unset_env("OPENAI_KEY");
}

TEST_CASE("LoadAgentsConfig reports error on missing file", "[agent_config]") {
  std::error_code ec;
  LoadAgentsConfig("/nonexistent/path/agents.json", ec);
  REQUIRE(ec);
  REQUIRE(ec == ConfigErrc::file_not_found);
}

TEST_CASE("LoadAgentsConfig reports error on invalid JSON", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write("not valid json");
  std::error_code ec;
  auto config = LoadAgentsConfig(tmp.path.string(), ec);
  REQUIRE(ec);
  REQUIRE(ec == ConfigErrc::parse_error);
}

TEST_CASE("LoadAgentsConfig works with explicit default_agent", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "only",
    "agents": [
      {
        "name": "only",
        "type": "llm",
        "backend": { "type": "ollama", "host": "http://localhost", "model": "x" }
      }
    ]
  })";
  tmp.write(json);
  std::error_code ec;
  AgentsConfig config = LoadAgentsConfig(tmp.path.string(), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(config.default_agent == "only");
}

TEST_CASE("SaveAgentsConfig writes valid JSON", "[agent_config]") {
  TempConfigFile tmp;
  AgentsConfig original;
  original.default_agent = "test";
  AgentEntry entry;
  entry.name = "test";
  entry.description = "desc";
  entry.backend.type = BackendType::kOpenAI;
  entry.backend.host = "https://api.test.com";
  entry.backend.model = "test-model";
  entry.backend.api_key = "secret";
  original.agents.push_back(entry);

  std::error_code ec;
  SaveAgentsConfig(tmp.path.string(), original, ec);
  REQUIRE_FALSE(ec);

  AgentsConfig loaded = LoadAgentsConfig(tmp.path.string(), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(loaded.default_agent == "test");
  REQUIRE(loaded.agents.size() == 1);
  REQUIRE(loaded.agents[0].name == "test");
  REQUIRE(loaded.agents[0].backend.type == BackendType::kOpenAI);
  REQUIRE(loaded.agents[0].backend.host == "https://api.test.com");
  REQUIRE(loaded.agents[0].backend.api_key == "secret");
}

TEST_CASE("CreateBackend creates OllamaBackend", "[agent_config]") {
  BackendConfig cfg;
  cfg.type = BackendType::kOllama;
  cfg.host = "http://localhost:11434";
  cfg.model = "llama3.2";
  cfg.temperature = 0.5f;
  cfg.system_prompt = "Be helpful.";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto adapter = std::make_unique<pu::backends::ollama::OllamaTokenAdapter>();
  std::error_code ec;
  auto backend = CreateBackend(cfg, std::move(mock_http), std::move(adapter), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(backend != nullptr);
}

TEST_CASE("CreateBackend creates OpenAIBackend", "[agent_config]") {
  BackendConfig cfg;
  cfg.type = BackendType::kOpenAI;
  cfg.host = "https://api.openai.com/v1";
  cfg.model = "gpt-4o-mini";
  cfg.api_key = "key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto adapter = std::make_unique<pu::backends::openai::OpenAITokenAdapter>();
  std::error_code ec;
  auto backend = CreateBackend(cfg, std::move(mock_http), std::move(adapter), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(backend != nullptr);
}

TEST_CASE("ExpandEnvVars warns on undefined variable", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "x",
    "agents": [
      {
        "name": "x",
        "type": "llm",
        "backend": {
          "type": "ollama",
          "host": "http://localhost:11434",
          "model": "llama3.2",
          "system_prompt": "${UNDEFINED_VAR_FOR_TEST}"
        }
      }
    ]
  })";
  tmp.write(json);
  std::error_code ec;
  AgentsConfig config = LoadAgentsConfig(tmp.path.string(), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(config.agents[0].backend.system_prompt.has_value());
  REQUIRE(config.agents[0].backend.system_prompt->empty());
}
