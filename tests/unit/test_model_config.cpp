// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "pu/model_config.hpp"
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

// ----------------------------------------------------------------------------
// Cross-platform environment variable helpers for testing
// ----------------------------------------------------------------------------
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

// ----------------------------------------------------------------------------
// Helper to create a temporary config file
// ----------------------------------------------------------------------------
struct TempConfigFile {
  fs::path path;
  TempConfigFile() {
    path = fs::temp_directory_path() / "pu_test_models.json";
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

// ----------------------------------------------------------------------------
// Tests
// ----------------------------------------------------------------------------
TEST_CASE("LoadModelsConfig parses valid JSON", "[model_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_model": "local",
    "models": [
      {
        "name": "local",
        "description": "Local Ollama",
        "backend": {
          "type": "ollama",
          "host": "http://localhost:11434",
          "model": "qwen3.5:4b",
          "temperature": 0.7
        }
      },
      {
        "name": "gpt",
        "description": "OpenAI",
        "backend": {
          "type": "openai",
          "host": "https://api.openai.com",
          "api_key": "${OPENAI_KEY}",
          "model": "gpt-4o-mini"
        }
      }
    ]
  })";
  tmp.write(json);

  // Set environment variable for test
  set_env("OPENAI_KEY", "test-key-123");

  ModelsFile models = LoadModelsConfig(tmp.path.string());

  REQUIRE(models.default_model == "local");
  REQUIRE(models.models.size() == 2);
  REQUIRE(models.models[0].name == "local");
  REQUIRE(models.models[0].backend.type == BackendType::kOllama);
  REQUIRE(models.models[1].name == "gpt");
  REQUIRE(models.models[1].backend.type == BackendType::kOpenAI);
  REQUIRE(models.models[1].backend.api_key == "test-key-123");

  unset_env("OPENAI_KEY");
}

TEST_CASE("LoadModelsConfig throws on missing file", "[model_config]") {
  REQUIRE_THROWS_AS(LoadModelsConfig("/nonexistent/path/models.json"), std::runtime_error);
}

TEST_CASE("LoadModelsConfig throws on invalid JSON", "[model_config]") {
  TempConfigFile tmp;
  tmp.write("not valid json");
  REQUIRE_THROWS_AS(LoadModelsConfig(tmp.path.string()), std::runtime_error);
}

TEST_CASE("LoadModelsConfig defaults model if empty", "[model_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "models": [
      {
        "name": "only",
        "backend": { "type": "ollama", "host": "http://localhost", "model": "x" }
      }
    ]
  })";
  tmp.write(json);
  ModelsFile models = LoadModelsConfig(tmp.path.string());
  REQUIRE(models.default_model == "only");
}

TEST_CASE("SaveModelsConfig writes valid JSON", "[model_config]") {
  TempConfigFile tmp;
  ModelsFile original;
  original.default_model = "test";
  ModelEntry entry;
  entry.name = "test";
  entry.description = "desc";
  entry.backend.type = BackendType::kOpenAI;
  entry.backend.host = "https://api.test.com";
  entry.backend.model = "test-model";
  entry.backend.api_key = "secret";
  original.models.push_back(entry);

  SaveModelsConfig(tmp.path.string(), original);

  // Read back and verify
  ModelsFile loaded = LoadModelsConfig(tmp.path.string());
  REQUIRE(loaded.default_model == "test");
  REQUIRE(loaded.models.size() == 1);
  REQUIRE(loaded.models[0].name == "test");
  REQUIRE(loaded.models[0].backend.type == BackendType::kOpenAI);
  REQUIRE(loaded.models[0].backend.host == "https://api.test.com");
  REQUIRE(loaded.models[0].backend.api_key == "secret");
}

TEST_CASE("CreateBackend creates OllamaBackend", "[model_config]") {
  BackendConfig cfg;
  cfg.type = BackendType::kOllama;
  cfg.host = "http://localhost:11434";
  cfg.model = "llama3.2";
  cfg.temperature = 0.5f;
  cfg.system_prompt = "Be helpful.";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto backend = CreateBackend(cfg, std::move(mock_http));
  REQUIRE(backend != nullptr);
  // We can't easily inspect the concrete type without RTTI,
  // but we can verify it doesn't throw.
}

TEST_CASE("CreateBackend creates OpenAIBackend", "[model_config]") {
  BackendConfig cfg;
  cfg.type = BackendType::kOpenAI;
  cfg.host = "https://api.openai.com";
  cfg.model = "gpt-4o-mini";
  cfg.api_key = "key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto backend = CreateBackend(cfg, std::move(mock_http));
  REQUIRE(backend != nullptr);
}

TEST_CASE("ExpandEnvVars warns on undefined variable", "[model_config]") {
  // This is tested indirectly via loading; the warning goes to cerr.
  // We just ensure the behavior doesn't crash.
  TempConfigFile tmp;
  std::string json = R"({
    "models": [
      {
        "name": "x",
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
  ModelsFile models = LoadModelsConfig(tmp.path.string());
  REQUIRE(models.models[0].backend.system_prompt.has_value());
  REQUIRE(models.models[0].backend.system_prompt->empty());  // expands to empty
}
