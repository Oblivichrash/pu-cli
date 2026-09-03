// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pu/llm/llm_provider.hpp"
#include "pu/http_client.hpp"
#include "pu/mcp/mcp_client.hpp"

namespace pu::config {

enum class BackendType { kOllama, kOpenAI };

struct SecurityPolicy {
  std::string sandbox_root;
  std::vector<std::string> allowed_paths;
  size_t max_command_length = 0;
  std::vector<std::string> forbidden_patterns;
};

struct BackendConfig {
  BackendType type = BackendType::kOllama;
  std::string host;
  std::string model;
  std::optional<std::string> api_key;
  float temperature = 0.7f;
  std::optional<std::string> system_prompt;
  bool parameters_as_string = false;
  int max_tokens = 2048;
  bool enable_thinking = true;  // for DeepSeek/vLLM only
};

// Keeps the old SessionBackendConfig JSON format (type as string, api_key as string).
inline void to_json(nlohmann::json& j, const BackendConfig& cfg) {
  j = nlohmann::json{
    {"type", cfg.type == BackendType::kOpenAI ? "openai" : "ollama"},
    {"host", cfg.host},
    {"model", cfg.model},
    {"api_key", cfg.api_key.value_or("")},
    {"temperature", cfg.temperature},
    {"max_tokens", cfg.max_tokens},
    {"parameters_as_string", cfg.parameters_as_string},
  };
}

inline void from_json(const nlohmann::json& j, BackendConfig& cfg) {
  auto type_str = j.value("type", "ollama");
  cfg.type = (type_str == "openai") ? BackendType::kOpenAI : BackendType::kOllama;
  cfg.host = j.value("host", "");
  cfg.model = j.value("model", "");
  if (j.contains("api_key") && j["api_key"].is_string()) {
    auto key = j["api_key"].get<std::string>();
    cfg.api_key = key.empty() ? std::optional<std::string>{} : key;
  }
  cfg.temperature = j.value("temperature", 0.7f);
  cfg.max_tokens = j.value("max_tokens", 2048);
  cfg.parameters_as_string = j.value("parameters_as_string", false);
  // The following fields may not be present in old session files; defaults are fine.
  cfg.system_prompt = std::nullopt;
  cfg.enable_thinking = true;
}

struct HistoryCompactionConfig {
  bool enabled = true;
  size_t keep_head = 10;
  size_t keep_tail = 50;
  std::string strategy = "truncate";  // reserved for future
};

struct AgentEntry {
  std::string name;
  std::string description;
  BackendConfig backend;
  std::vector<std::string> tools;
  SecurityPolicy security;
  std::vector<pu::mcp::McpServerConfig> mcp_servers;
  HistoryCompactionConfig compaction;
};

struct AgentsConfig {
  std::string default_agent;
  std::vector<AgentEntry> agents;
};

std::string FindConfigPath();
AgentsConfig LoadAgentsConfig(const std::string& config_path);
void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& config);
std::unique_ptr<pu::LLMProvider> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http);

}  // namespace pu::config