// SPDX-License-Identifier: GPL-3.0-only

#include "pu/expert_config.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/backend.hpp"
#include "pu/error_codes.hpp"
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

  std::error_code ec;
  ExpertsConfig config = LoadExpertsConfig(tmp.path.string(), ec);
  REQUIRE_FALSE(ec);

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

TEST_CASE("LoadExpertsConfig reports error on missing file", "[expert_config]") {
  std::error_code ec;
  LoadExpertsConfig("/nonexistent/path/experts.json", ec);
  REQUIRE(ec);
  REQUIRE(ec == ConfigErrc::file_not_found);
}

TEST_CASE("LoadExpertsConfig reports error on invalid JSON", "[expert_config]") {
  TempConfigFile tmp;
  tmp.write("not valid json");
  std::error_code ec;
  auto config = LoadExpertsConfig(tmp.path.string(), ec);
  REQUIRE(ec);
  REQUIRE(ec == ConfigErrc::parse_error);
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
  std::error_code ec;
  ExpertsConfig config = LoadExpertsConfig(tmp.path.string(), ec);
  REQUIRE_FALSE(ec);
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

  std::error_code ec;
  SaveExpertsConfig(tmp.path.string(), original, ec);
  REQUIRE_FALSE(ec);

  ExpertsConfig loaded = LoadExpertsConfig(tmp.path.string(), ec);
  REQUIRE_FALSE(ec);
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
  std::error_code ec;
  auto backend = CreateBackend(cfg, std::move(mock_http), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(backend != nullptr);
}

TEST_CASE("CreateBackend creates OpenAIBackend", "[expert_config]") {
  BackendConfig cfg;
  cfg.type = BackendType::kOpenAI;
  cfg.host = "https://api.openai.com";
  cfg.model = "gpt-4o-mini";
  cfg.api_key = "key";

  auto mock_http = std::make_unique<MockHttpClient>();
  std::error_code ec;
  auto backend = CreateBackend(cfg, std::move(mock_http), ec);
  REQUIRE_FALSE(ec);
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
  std::error_code ec;
  ExpertsConfig config = LoadExpertsConfig(tmp.path.string(), ec);
  REQUIRE_FALSE(ec);
  REQUIRE(config.experts[0].backend.system_prompt.has_value());
  REQUIRE(config.experts[0].backend.system_prompt->empty());
}
