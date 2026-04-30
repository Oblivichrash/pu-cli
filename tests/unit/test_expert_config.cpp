// SPDX-License-Identifier: GPL-3.0-only

#include "pu/expert_config.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/backend.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <cstring>
#endif

using namespace pu::config;
using namespace pu::tests;

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
    path = fs::temp_directory_path() / "pu_test_experts.json";
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

TEST_CASE("LoadExpertsConfig parses valid JSON", "[expert_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_expert": "chat",
    "experts": [
      {
        "name": "chat",
        "type": "chat",
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
        "type": "bash",
        "description": "Command Runner",
        "backend": {
          "type": "openai",
          "host": "https://api.openai.com",
          "api_key": "${OPENAI_KEY}",
          "model": "gpt-4o-mini"
        },
        "executor": {
          "sandbox": "/tmp"
        }
      }
    ]
  })";
  tmp.write(json);

  set_env("OPENAI_KEY", "test-key-123");

  ExpertsConfig config = LoadExpertsConfig(tmp.path.string());

  REQUIRE(config.default_expert == "chat");
  REQUIRE(config.experts.size() == 2);
  REQUIRE(config.experts[0].name == "chat");
  REQUIRE(config.experts[0].type == ExpertType::kChat);
  REQUIRE(config.experts[0].backend.type == BackendType::kOllama);
  REQUIRE(config.experts[1].name == "bash");
  REQUIRE(config.experts[1].type == ExpertType::kBash);
  REQUIRE(config.experts[1].backend.type == BackendType::kOpenAI);
  REQUIRE(config.experts[1].backend.api_key == "test-key-123");
  REQUIRE(config.experts[1].sandbox_path == "/tmp");

  unset_env("OPENAI_KEY");
}

TEST_CASE("LoadExpertsConfig throws on missing file", "[expert_config]") {
  REQUIRE_THROWS_AS(LoadExpertsConfig("/nonexistent/path/experts.json"), std::runtime_error);
}

TEST_CASE("LoadExpertsConfig throws on invalid JSON", "[expert_config]") {
  TempConfigFile tmp;
  tmp.write("not valid json");
  REQUIRE_THROWS_AS(LoadExpertsConfig(tmp.path.string()), std::runtime_error);
}

TEST_CASE("LoadExpertsConfig defaults expert if empty", "[expert_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "experts": [
      {
        "name": "only",
        "type": "chat",
        "backend": { "type": "ollama", "host": "http://localhost", "model": "x" }
      }
    ]
  })";
  tmp.write(json);
  ExpertsConfig config = LoadExpertsConfig(tmp.path.string());
  REQUIRE(config.default_expert == "only");
}

TEST_CASE("SaveExpertsConfig writes valid JSON", "[expert_config]") {
  TempConfigFile tmp;
  ExpertsConfig original;
  original.default_expert = "test";
  ExpertEntry entry;
  entry.name = "test";
  entry.type = ExpertType::kChat;
  entry.description = "desc";
  entry.backend.type = BackendType::kOpenAI;
  entry.backend.host = "https://api.test.com";
  entry.backend.model = "test-model";
  entry.backend.api_key = "secret";
  original.experts.push_back(entry);

  SaveExpertsConfig(tmp.path.string(), original);

  ExpertsConfig loaded = LoadExpertsConfig(tmp.path.string());
  REQUIRE(loaded.default_expert == "test");
  REQUIRE(loaded.experts.size() == 1);
  REQUIRE(loaded.experts[0].name == "test");
  REQUIRE(loaded.experts[0].type == ExpertType::kChat);
  REQUIRE(loaded.experts[0].backend.type == BackendType::kOpenAI);
  REQUIRE(loaded.experts[0].backend.host == "https://api.test.com");
  REQUIRE(loaded.experts[0].backend.api_key == "secret");
}

TEST_CASE("CreateBackend creates OllamaBackend", "[expert_config]") {
  BackendConfig cfg;
  cfg.type = BackendType::kOllama;
  cfg.host = "http://localhost:11434";
  cfg.model = "llama3.2";
  cfg.temperature = 0.5f;
  cfg.system_prompt = "Be helpful.";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto backend = CreateBackend(cfg, std::move(mock_http));
  REQUIRE(backend != nullptr);
}

TEST_CASE("CreateBackend creates OpenAIBackend", "[expert_config]") {
  BackendConfig cfg;
  cfg.type = BackendType::kOpenAI;
  cfg.host = "https://api.openai.com";
  cfg.model = "gpt-4o-mini";
  cfg.api_key = "key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto backend = CreateBackend(cfg, std::move(mock_http));
  REQUIRE(backend != nullptr);
}

TEST_CASE("ExpandEnvVars warns on undefined variable", "[expert_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "experts": [
      {
        "name": "x",
        "type": "chat",
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
  ExpertsConfig config = LoadExpertsConfig(tmp.path.string());
  REQUIRE(config.experts[0].backend.system_prompt.has_value());
  REQUIRE(config.experts[0].backend.system_prompt->empty());
}
