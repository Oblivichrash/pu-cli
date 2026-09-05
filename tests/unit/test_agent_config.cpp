// SPDX-License-Identifier: GPL-3.0-only

#include "pu/agent_config.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/error.hpp"
#include "pu/json.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <cstring>
#endif

using namespace pu;
using namespace pu::tests;

namespace fs = std::filesystem;

#ifdef _WIN32
static void set_env(const char* name, const char* value) {
    std::string s = std::string(name) + "=" + std::string(value);
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


namespace {

// Test-only stand-in for the removed config::SaveAgentsConfig. It keeps the
// agents.json loader round-trip tests meaningful without shipping an unused
// production writer (HistoryCompactionConfig no longer carries a strategy).
void WriteAgentsConfigForTest(const std::string& config_path,
                              const config::AgentsConfig& cfg) {
  json::value j = {{"default_agent", cfg.default_agent}};

  json::array agents_array;
  for (const auto& entry : cfg.agents) {
    json::value item = {
      {"name", entry.name},
      {"description", entry.description},
    };
    item.as_object()["tools"] = boost::json::value_from(entry.tools);

    json::value security = {
      {"sandbox_root", entry.security.sandbox_root},
      {"max_command_length", entry.security.max_command_length},
    };
    security.as_object()["allowed_paths"] =
        boost::json::value_from(entry.security.allowed_paths);
    security.as_object()["forbidden_patterns"] =
        boost::json::value_from(entry.security.forbidden_patterns);
    item.as_object()["security"] = security;

    json::value backend = {
      {"type", (entry.backend.type == config::BackendType::kOpenAI) ? "openai" : "ollama"},
      {"host", entry.backend.host},
      {"model", entry.backend.model},
      {"temperature", entry.backend.temperature},
      {"enable_thinking", entry.backend.enable_thinking},
    };
    if (entry.backend.api_key) backend.as_object()["api_key"] = *entry.backend.api_key;
    if (entry.backend.system_prompt)
      backend.as_object()["system_prompt"] = *entry.backend.system_prompt;
    item.as_object()["backend"] = backend;

    if (!entry.mcp_servers.empty()) {
      json::array mcp_array;
      for (const auto& srv : entry.mcp_servers) {
        json::value srv_json = {
          {"name", srv.name},
          {"command", srv.command},
        };
        srv_json.as_object()["args"] = boost::json::value_from(srv.args);
        if (!srv.url.empty()) srv_json.as_object()["url"] = srv.url;
        if (!srv.headers.empty()) {
          json::value headers = json::object{};
          for (const auto& [k, v] : srv.headers) headers.as_object()[k] = v;
          srv_json.as_object()["headers"] = headers;
        }
        mcp_array.push_back(srv_json);
      }
      item.as_object()["mcp_servers"] = mcp_array;
    }

    json::value compaction = {
      {"enabled", entry.compaction.enabled},
      {"keep_head", entry.compaction.keep_head},
      {"keep_tail", entry.compaction.keep_tail},
    };
    item.as_object()["history_compaction"] = compaction;

    agents_array.push_back(item);
  }
  j.as_object()["agents"] = agents_array;

  std::ofstream file(config_path);
  if (!file.is_open())
    throw pu::Error("Failed to open config file for writing: " + config_path);
  file << json::PrettyPrint(j);
}

}  // namespace

TEST_CASE("FindConfigPath prefers ./.pu/agents.json", "[agent_config]") {
  // Use a temporary current directory and empty HOME so we do not disturb the
  // repo layout or the real user configuration.
  auto dir = fs::temp_directory_path() / "pu_findconfig_project";
  auto home = fs::temp_directory_path() / "pu_findconfig_home";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
  fs::create_directories(dir / ".pu");
  fs::create_directories(home);

  auto old = fs::current_path();
  fs::current_path(dir);
  set_env("HOME", home.string().c_str());

  {
    std::ofstream f(dir / ".pu" / "agents.json");
    f << "{}";
  }

  REQUIRE(config::FindConfigPath() == "./.pu/agents.json");

  fs::current_path(old);
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
}

TEST_CASE("FindConfigPath falls back to ~/.pu/agents.json", "[agent_config]") {
  auto dir = fs::temp_directory_path() / "pu_findconfig_project2";
  auto home = fs::temp_directory_path() / "pu_findconfig_home2";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
  fs::create_directories(dir);
  fs::create_directories(home / ".pu");

  auto old = fs::current_path();
  fs::current_path(dir);
  set_env("HOME", home.string().c_str());

  {
    std::ofstream f(home / ".pu" / "agents.json");
    f << "{}";
  }

  REQUIRE(config::FindConfigPath() == (home / ".pu" / "agents.json").string());

  fs::current_path(old);
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
}

TEST_CASE("FindConfigPath throws when neither location exists", "[agent_config]") {
  auto dir = fs::temp_directory_path() / "pu_findconfig_empty";
  auto home = fs::temp_directory_path() / "pu_findconfig_empty_home";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
  fs::create_directories(dir);
  fs::create_directories(home);

  auto old = fs::current_path();
  fs::current_path(dir);
  set_env("HOME", home.string().c_str());

  REQUIRE_THROWS_AS(config::FindConfigPath(), std::runtime_error);

  fs::current_path(old);
  fs::remove_all(dir, ec);
  fs::remove_all(home, ec);
}

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

  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());

  REQUIRE(cfg.default_agent == "chat");
  REQUIRE(cfg.agents.size() == 2);
  REQUIRE(cfg.agents[0].name == "chat");
  REQUIRE(cfg.agents[0].backend.type == config::BackendType::kOllama);
  REQUIRE(cfg.agents[1].name == "bash");
  REQUIRE(cfg.agents[1].backend.type == config::BackendType::kOpenAI);
  REQUIRE(cfg.agents[1].backend.api_key == "test-key-123");
  REQUIRE(cfg.agents[1].security.sandbox_root == "/tmp");

  unset_env("OPENAI_KEY");
}

TEST_CASE("LoadAgentsConfig throws on missing file", "[agent_config]") {
  REQUIRE_THROWS_AS(config::LoadAgentsConfig("/nonexistent/path/agents.json"), std::runtime_error);
}

TEST_CASE("LoadAgentsConfig throws on invalid JSON", "[agent_config]") {
  TempConfigFile tmp;
  tmp.write("not valid json");
  REQUIRE_THROWS_AS(config::LoadAgentsConfig(tmp.path.string()), std::runtime_error);
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
  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.default_agent == "only");
}

TEST_CASE("Agents config writer produces valid JSON", "[agent_config]") {
  TempConfigFile tmp;
  config::AgentsConfig original;
  original.default_agent = "test";
  config::AgentEntry entry;
  entry.name = "test";
  entry.description = "desc";
  entry.backend.type = config::BackendType::kOpenAI;
  entry.backend.host = "https://api.test.com";
  entry.backend.model = "test-model";
  entry.backend.api_key = "secret";
  original.agents.push_back(entry);

  REQUIRE_NOTHROW(WriteAgentsConfigForTest(tmp.path.string(), original));

  config::AgentsConfig loaded = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(loaded.default_agent == "test");
  REQUIRE(loaded.agents.size() == 1);
  REQUIRE(loaded.agents[0].name == "test");
  REQUIRE(loaded.agents[0].backend.type == config::BackendType::kOpenAI);
  REQUIRE(loaded.agents[0].backend.host == "https://api.test.com");
  REQUIRE(loaded.agents[0].backend.api_key == "secret");
}

TEST_CASE("CreateBackend creates OllamaBackend", "[agent_config]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kOllama;
  cfg.host = "http://localhost:11434";
  cfg.model = "llama3.2";
  cfg.temperature = 0.5f;
  cfg.system_prompt = "Be helpful.";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto backend = config::CreateBackend(cfg, std::move(mock_http));
  REQUIRE(backend != nullptr);
}

TEST_CASE("CreateBackend creates OpenAIBackend", "[agent_config]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kOpenAI;
  cfg.host = "https://api.openai.com/v1";
  cfg.model = "gpt-4o-mini";
  cfg.api_key = "key";

  auto mock_http = std::make_unique<MockHttpClient>();
  auto backend = config::CreateBackend(cfg, std::move(mock_http));
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
  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents[0].backend.system_prompt.has_value());
  REQUIRE(cfg.agents[0].backend.system_prompt->empty());
}

TEST_CASE("LoadAgentsConfig parses enable_thinking and history_compaction", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "deepseek",
    "agents": [
      {
        "name": "deepseek",
        "backend": {
          "type": "openai",
          "host": "https://api.deepseek.com/v1",
          "model": "deepseek-reasoner",
          "enable_thinking": true
        },
        "history_compaction": {
          "enabled": false,
          "keep_head": 15,
          "keep_tail": 60
        }
      }
    ]
  })";
  tmp.write(json);
  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents.size() == 1);
  REQUIRE(cfg.agents[0].backend.enable_thinking == true);
  REQUIRE(cfg.agents[0].compaction.enabled == false);
  REQUIRE(cfg.agents[0].compaction.keep_head == 15);
  REQUIRE(cfg.agents[0].compaction.keep_tail == 60);
}

TEST_CASE("LoadAgentsConfig uses defaults when compaction fields absent", "[agent_config]") {
  TempConfigFile tmp;
  std::string json = R"({
    "default_agent": "chat",
    "agents": [
      {
        "name": "chat",
        "backend": { "type": "ollama", "host": "http://localhost:11434", "model": "qwen" }
      }
    ]
  })";
  tmp.write(json);
  config::AgentsConfig cfg = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents[0].backend.enable_thinking == true);
  REQUIRE(cfg.agents[0].compaction.enabled == true);
  REQUIRE(cfg.agents[0].compaction.keep_head == 10);
  REQUIRE(cfg.agents[0].compaction.keep_tail == 50);
}

TEST_CASE("Agents config writer round-trips enable_thinking and compaction", "[agent_config]") {
  TempConfigFile tmp;
  config::AgentsConfig original;
  original.default_agent = "deepseek";
  config::AgentEntry entry;
  entry.name = "deepseek";
  entry.backend.type = config::BackendType::kOpenAI;
  entry.backend.host = "https://api.deepseek.com/v1";
  entry.backend.model = "deepseek-reasoner";
  entry.backend.enable_thinking = true;
  entry.compaction.enabled = false;
  entry.compaction.keep_head = 15;
  entry.compaction.keep_tail = 60;
  original.agents.push_back(entry);

  REQUIRE_NOTHROW(WriteAgentsConfigForTest(tmp.path.string(), original));

  config::AgentsConfig loaded = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(loaded.agents.size() == 1);
  REQUIRE(loaded.agents[0].backend.enable_thinking == true);
  REQUIRE(loaded.agents[0].compaction.enabled == false);
  REQUIRE(loaded.agents[0].compaction.keep_head == 15);
  REQUIRE(loaded.agents[0].compaction.keep_tail == 60);
}
