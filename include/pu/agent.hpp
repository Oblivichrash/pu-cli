// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "pu/core/http_client.hpp"
#include "pu/core/json.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/mcp/client.hpp"

namespace pu::config {

enum class BackendType { kOllama, kOpenAI, kCodeBuddy };

struct SecurityPolicy {
  std::string sandbox_root;
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
  int max_tokens = 2048;
  // How much reasoning to ask for. The default asks for nothing, which is what
  // "on" meant before there was a level.
  ThinkingLevel thinking = ThinkingLevel::kServerDefault;
};

// How a type is spelled in configuration and on the wire. One home, because the
// stored session, an agents.json entry and an API response all name it, and a
// third type is what turns every `is it OpenAI?` question into a lie: each of
// those places used to answer itself and write "ollama" for anything else.
inline const char* BackendTypeName(BackendType type) {
  switch (type) {
    case BackendType::kOllama:
      return "ollama";
    case BackendType::kOpenAI:
      return "openai";
    case BackendType::kCodeBuddy:
      return "codebuddy";
  }
  return "ollama";
}

inline std::optional<BackendType> ParseBackendType(std::string_view name) {
  if (name == "ollama") return BackendType::kOllama;
  if (name == "openai") return BackendType::kOpenAI;
  if (name == "codebuddy") return BackendType::kCodeBuddy;
  return std::nullopt;
}

inline void tag_invoke(boost::json::value_from_tag, boost::json::value& j,
                       const BackendConfig& cfg) {
  j = {
      {"type", BackendTypeName(cfg.type)},
      {"host", cfg.host},
      {"model", cfg.model},
      {"api_key", cfg.api_key.value_or("")},
      {"temperature", cfg.temperature},
      {"max_tokens", cfg.max_tokens},
      {"thinking", ThinkingLevelName(cfg.thinking)},
  };
}

inline BackendConfig tag_invoke(boost::json::value_to_tag<BackendConfig>,
                                const boost::json::value& j) {
  BackendConfig cfg;
  const auto type_str = json::ValueOrDefault<std::string>(j, "type", "ollama");
  // A file naming something this build does not know reads as the default rather
  // than failing the load: the session it holds is still the user's conversation.
  cfg.type = ParseBackendType(type_str).value_or(BackendType::kOllama);
  cfg.host = json::ValueOrDefault<std::string>(j, "host", "");
  cfg.model = json::ValueOrDefault<std::string>(j, "model", "");
  if (json::HasKey(j, "api_key") && j.at("api_key").is_string()) {
    auto key = boost::json::value_to<std::string>(j.at("api_key"));
    cfg.api_key = key.empty() ? std::optional<std::string>{} : key;
  }
  cfg.temperature = json::ValueOrDefault<float>(j, "temperature", 0.7f);
  cfg.max_tokens = json::ValueOrDefault<int>(j, "max_tokens", 2048);
  // The prompt is configuration, not session state, so a stored override never
  // carries one.
  cfg.system_prompt = std::nullopt;
  cfg.thinking = ReadThinkingLevel(j);
  return cfg;
}

struct AgentEntry {
  std::string name;
  std::string description;
  BackendConfig backend;
  SecurityPolicy security;
  std::vector<pu::mcp::McpServerConfig> mcp_servers;
};

struct AgentsConfig {
  std::string default_agent;
  std::vector<AgentEntry> agents;
};

std::string FindConfigPath();
AgentsConfig LoadAgentsConfig(const std::string& config_path);
std::unique_ptr<pu::LLMProvider> CreateBackend(const BackendConfig& cfg,
                                               std::unique_ptr<pu::http::HttpClient> http);

}  // namespace pu::config

namespace pu {

// The configured agents, and which one of them is active.
class AgentManager {
 public:
  AgentManager();

  void LoadAgentConfigs(const std::vector<config::AgentEntry>& configs);

  const config::AgentEntry* GetAgentConfig(const std::string& name) const;

  std::vector<std::string> GetAgentNames() const;

  void SetActiveAgent(const std::string& name);
  std::string GetActiveAgent() const;

 private:
  std::string active_agent_;

  std::vector<config::AgentEntry> agent_configs_;
};

}  // namespace pu
