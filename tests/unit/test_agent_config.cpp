// SPDX-License-Identifier: GPL-3.0-only

#include "pu/agent_config.hpp"
#include "pu/llm/provider_factory.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "pu/error.hpp"
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
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

TEST_CASE("SaveAgentsConfig writes valid JSON", "[agent_config]") {
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

  REQUIRE_NOTHROW(config::SaveAgentsConfig(tmp.path.string(), original));

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
          "keep_tail": 60,
          "strategy": "truncate"
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
  REQUIRE(cfg.agents[0].compaction.strategy == "truncate");
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
  REQUIRE(cfg.agents[0].compaction.strategy == "truncate");
}

TEST_CASE("SaveAgentsConfig round-trips enable_thinking and compaction", "[agent_config]") {
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
  entry.compaction.strategy = "truncate";
  original.agents.push_back(entry);

  REQUIRE_NOTHROW(config::SaveAgentsConfig(tmp.path.string(), original));

  config::AgentsConfig loaded = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(loaded.agents.size() == 1);
  REQUIRE(loaded.agents[0].backend.enable_thinking == true);
  REQUIRE(loaded.agents[0].compaction.enabled == false);
  REQUIRE(loaded.agents[0].compaction.keep_head == 15);
  REQUIRE(loaded.agents[0].compaction.keep_tail == 60);
  REQUIRE(loaded.agents[0].compaction.strategy == "truncate");
}

TEST_CASE("BackendConfig to_json omits reasoning_effort when unset", "[agent_config]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kOpenAI;
  cfg.host = "https://api.test.com/v1";
  cfg.model = "m";
  nlohmann::json j = cfg;
  REQUIRE_FALSE(j.contains("reasoning_effort"));
}

TEST_CASE("BackendConfig to_json emits reasoning_effort when set", "[agent_config]") {
  config::BackendConfig cfg;
  cfg.type = config::BackendType::kOpenAI;
  cfg.host = "https://api.test.com/v1";
  cfg.model = "m";
  cfg.reasoning_effort = "high";
  nlohmann::json j = cfg;
  REQUIRE(j["reasoning_effort"] == "high");
}

TEST_CASE("BackendConfig from_json handles reasoning_effort", "[agent_config]") {
  nlohmann::json j = nlohmann::json::object();
  j["type"] = "openai";
  j["host"] = "https://api.test.com/v1";
  j["model"] = "m";
  j["reasoning_effort"] = "medium";
  auto cfg = j.get<config::BackendConfig>();
  REQUIRE(cfg.reasoning_effort.has_value());
  REQUIRE(*cfg.reasoning_effort == "medium");

  // Absent field -> nullopt (old agents.json keeps loading).
  nlohmann::json j2 = nlohmann::json::object();
  j2["type"] = "openai";
  j2["host"] = "https://api.test.com/v1";
  j2["model"] = "m";
  auto cfg2 = j2.get<config::BackendConfig>();
  REQUIRE_FALSE(cfg2.reasoning_effort.has_value());
}

TEST_CASE("SaveAgentsConfig round-trips reasoning_effort", "[agent_config]") {
  TempConfigFile tmp;
  config::AgentsConfig original;
  original.default_agent = "deepseek";
  config::AgentEntry entry;
  entry.name = "deepseek";
  entry.backend.type = config::BackendType::kOpenAI;
  entry.backend.host = "https://api.deepseek.com/v1";
  entry.backend.model = "deepseek-reasoner";
  entry.backend.enable_thinking = true;
  entry.backend.reasoning_effort = "high";
  original.agents.push_back(entry);

  REQUIRE_NOTHROW(config::SaveAgentsConfig(tmp.path.string(), original));
  auto loaded = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(loaded.agents[0].backend.reasoning_effort.has_value());
  REQUIRE(*loaded.agents[0].backend.reasoning_effort == "high");

  // Round-trip again to ensure serialization is stable.
  config::SaveAgentsConfig(tmp.path.string(), loaded);
  auto loaded2 = config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(loaded2.agents[0].backend.reasoning_effort.has_value());
  REQUIRE(*loaded2.agents[0].backend.reasoning_effort == "high");
}

TEST_CASE("AgentEntry full Save->Load round-trip preserves all fields", "[agent_config]") {
  TempConfigFile tmp;
  config::AgentsConfig original;
  original.default_agent = "full";

  config::AgentEntry entry;
  entry.name = "full";
  entry.description = "round-trip coverage";
  entry.backend.type = config::BackendType::kOpenAI;
  entry.backend.host = "https://api.test.com/v1";
  entry.backend.model = "model-x";
  entry.backend.enable_thinking = true;
  entry.backend.reasoning_effort = "high";

  entry.tools = {"execute_bash", "write_file", "ask_user"};
  entry.security.sandbox_root = "/srv/work";
  entry.security.allowed_paths = {"/srv/work", "/tmp/shared"};
  entry.security.max_command_length = 8192;
  entry.security.forbidden_patterns = {"rm -rf", "sudo", "mkfs"};

  pu::mcp::McpServerConfig fs;
  fs.name = "filesystem";
  fs.command = "npx";
  fs.args = {"-y", "@modelcontextprotocol/server-filesystem", "/srv/work"};
  pu::mcp::McpServerConfig git;
  git.name = "git";
  git.command = "mcp-git";
  git.args = {"--verbose"};
  entry.mcp_servers = {fs, git};

  entry.compaction.enabled = false;
  entry.compaction.keep_head = 20;
  entry.compaction.keep_tail = 80;
  entry.compaction.strategy = "summary";  // non-default strategy

  original.agents.push_back(entry);

  REQUIRE_NOTHROW(config::SaveAgentsConfig(tmp.path.string(), original));
  auto loaded = config::LoadAgentsConfig(tmp.path.string());

  REQUIRE(loaded.agents.size() == 1);
  const auto& a = loaded.agents[0];
  REQUIRE(a.name == "full");
  REQUIRE(a.description == "round-trip coverage");

  // tools
  REQUIRE(a.tools.size() == 3);
  REQUIRE(a.tools[0] == "execute_bash");
  REQUIRE(a.tools[1] == "write_file");
  REQUIRE(a.tools[2] == "ask_user");

  // security
  REQUIRE(a.security.sandbox_root == "/srv/work");
  REQUIRE(a.security.allowed_paths.size() == 2);
  REQUIRE(a.security.allowed_paths[0] == "/srv/work");
  REQUIRE(a.security.allowed_paths[1] == "/tmp/shared");
  REQUIRE(a.security.max_command_length == 8192);
  REQUIRE(a.security.forbidden_patterns.size() == 3);
  REQUIRE(a.security.forbidden_patterns[1] == "sudo");
  REQUIRE(a.security.forbidden_patterns[2] == "mkfs");

  // mcp_servers (multiple, with args)
  REQUIRE(a.mcp_servers.size() == 2);
  REQUIRE(a.mcp_servers[0].name == "filesystem");
  REQUIRE(a.mcp_servers[0].command == "npx");
  REQUIRE(a.mcp_servers[0].args.size() == 3);
  REQUIRE(a.mcp_servers[0].args[1] == "@modelcontextprotocol/server-filesystem");
  REQUIRE(a.mcp_servers[1].name == "git");
  REQUIRE(a.mcp_servers[1].command == "mcp-git");
  REQUIRE(a.mcp_servers[1].args.size() == 1);
  REQUIRE(a.mcp_servers[1].args[0] == "--verbose");

  // history_compaction with non-default strategy
  REQUIRE(a.compaction.enabled == false);
  REQUIRE(a.compaction.keep_head == 20);
  REQUIRE(a.compaction.keep_tail == 80);
  REQUIRE(a.compaction.strategy == "summary");

  // backend extras survive too
  REQUIRE(a.backend.reasoning_effort.has_value());
  REQUIRE(*a.backend.reasoning_effort == "high");
}
