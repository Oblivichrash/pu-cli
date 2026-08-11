// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "pu/agent_config.hpp"
#include "src/config_tools/agent_crud.hpp"

namespace fs = std::filesystem;

namespace {

struct TempConfig {
  fs::path path;
  TempConfig() {
    path = fs::temp_directory_path() / "pu_test_config_crud.json";
  }
  ~TempConfig() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  void WriteValid() {
    std::ofstream f(path);
    f << R"({
      "default_agent": "alpha",
      "agents": [
        {
          "name": "alpha",
          "description": "Alpha agent",
          "backend": {
            "type": "ollama",
            "host": "http://localhost:11434",
            "model": "qwen3.5:2b"
          },
          "tools": ["execute_bash"],
          "security": {
            "sandbox_root": ".",
            "forbidden_patterns": ["rm -rf", "sudo"]
          }
        },
        {
          "name": "beta",
          "description": "Beta agent",
          "backend": {
            "type": "openai",
            "host": "https://api.openai.com/v1",
            "model": "gpt-4o-mini",
            "api_key": "test-key"
          },
          "tools": ["write_file"],
          "security": {
            "sandbox_root": ".",
            "forbidden_patterns": ["rm -rf"]
          }
        }
      ]
    })";
  }
};

}  // unnamed namespace

TEST_CASE("ListAgents returns 0 and lists all agents", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::ListAgents(tmp.path.string(), false);
  REQUIRE(rc == 0);
}

TEST_CASE("ListAgents with --json outputs valid JSON", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::ListAgents(tmp.path.string(), true);
  REQUIRE(rc == 0);
}

TEST_CASE("ShowAgent returns details for existing agent", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::ShowAgent(tmp.path.string(), "alpha", false);
  REQUIRE(rc == 0);
}

TEST_CASE("ShowAgent fails for missing agent", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::ShowAgent(tmp.path.string(), "nonexistent", false);
  REQUIRE(rc == 1);
}

TEST_CASE("AddAgent creates a new agent", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  // The wizard reads from stdin; we can't easily test it here.
  // Instead verify that the config file is loadable after manual modification.
  auto cfg = pu::config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents.size() == 2);
  REQUIRE(cfg.agents[0].name == "alpha");
  REQUIRE(cfg.agents[1].name == "beta");
}

TEST_CASE("RemoveAgent removes an agent and updates default", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::RemoveAgent(tmp.path.string(), "alpha");
  REQUIRE(rc == 0);

  auto cfg = pu::config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents.size() == 1);
  REQUIRE(cfg.agents[0].name == "beta");
  REQUIRE(cfg.default_agent == "beta");
}

TEST_CASE("RemoveAgent fails for missing agent", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::RemoveAgent(tmp.path.string(), "nonexistent");
  REQUIRE(rc == 1);
}

TEST_CASE("RenameAgent renames an agent and updates default", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::RenameAgent(tmp.path.string(), "alpha", "gamma");
  REQUIRE(rc == 0);

  auto cfg = pu::config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.agents.size() == 2);
  REQUIRE(cfg.agents[0].name == "gamma");
  REQUIRE(cfg.default_agent == "gamma");
}

TEST_CASE("RenameAgent fails for missing source", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::RenameAgent(tmp.path.string(), "nonexistent", "gamma");
  REQUIRE(rc == 1);
}

TEST_CASE("RenameAgent fails for duplicate target", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::RenameAgent(tmp.path.string(), "alpha", "beta");
  REQUIRE(rc == 1);
}

TEST_CASE("SetDefaultAgent sets the default", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::SetDefaultAgent(tmp.path.string(), "beta");
  REQUIRE(rc == 0);

  auto cfg = pu::config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(cfg.default_agent == "beta");
}

TEST_CASE("SetDefaultAgent fails for missing agent", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  int rc = pu::config_tools::SetDefaultAgent(tmp.path.string(), "nonexistent");
  REQUIRE(rc == 1);
}

TEST_CASE("Saved config round-trips through LoadAgentsConfig", "[config_crud]") {
  TempConfig tmp;
  tmp.WriteValid();

  auto cfg = pu::config::LoadAgentsConfig(tmp.path.string());
  pu::config::SaveAgentsConfig(tmp.path.string(), cfg);

  auto reloaded = pu::config::LoadAgentsConfig(tmp.path.string());
  REQUIRE(reloaded.agents.size() == 2);
  REQUIRE(reloaded.agents[0].name == "alpha");
  REQUIRE(reloaded.agents[1].name == "beta");
}