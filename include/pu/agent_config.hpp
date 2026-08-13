// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pu/error.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/http_client.hpp"

// MCP servers are configured per-agent; the struct lives here (not in the MCP
// client header) so config parsing never drags in the MCP/LLM stack.
namespace pu::mcp {
struct McpServerConfig {
    std::string name;
    std::string command;
    std::vector<std::string> args;
};

inline void to_json(nlohmann::json& j, const McpServerConfig& cfg) {
  j = nlohmann::json{
      {"name", cfg.name},
      {"command", cfg.command},
      {"args", cfg.args},
  };
}

inline void from_json(const nlohmann::json& j, McpServerConfig& cfg) {
  cfg.name = j.value("name", "");
  cfg.command = j.value("command", "");
  cfg.args.clear();
  if (j.contains("args") && j["args"].is_array()) {
    for (const auto& a : j["args"])
      if (a.is_string()) cfg.args.push_back(a.get<std::string>());
  }
}
}  // namespace pu::mcp

namespace pu::config {

enum class BackendType { kOllama, kOpenAI };

struct SecurityPolicy {
  std::string sandbox_root;
  std::vector<std::string> allowed_paths;
  size_t max_command_length = 0;
  std::vector<std::string> forbidden_patterns;
};

inline void to_json(nlohmann::json& j, const SecurityPolicy& policy) {
  j = nlohmann::json{
      {"sandbox_root", policy.sandbox_root},
      {"allowed_paths", policy.allowed_paths},
      {"max_command_length", policy.max_command_length},
      {"forbidden_patterns", policy.forbidden_patterns},
  };
}

inline void from_json(const nlohmann::json& j, SecurityPolicy& policy) {
  policy.sandbox_root = j.value("sandbox_root", "");
  policy.allowed_paths.clear();
  if (j.contains("allowed_paths") && j["allowed_paths"].is_array()) {
    for (const auto& p : j["allowed_paths"])
      if (p.is_string()) policy.allowed_paths.push_back(p.get<std::string>());
  }
  policy.max_command_length = j.value("max_command_length", 0);
  policy.forbidden_patterns.clear();
  if (j.contains("forbidden_patterns") && j["forbidden_patterns"].is_array()) {
    for (const auto& pat : j["forbidden_patterns"])
      if (pat.is_string()) policy.forbidden_patterns.push_back(pat.get<std::string>());
  }
}

struct BackendConfig {
  BackendType type = BackendType::kOllama;
  std::string host;
  std::string model;
  std::optional<std::string> api_key;
  float temperature = 0.7f;
  std::optional<std::string> system_prompt;
  int max_tokens = 2048;
  bool enable_thinking = true;  // for DeepSeek/vLLM only
  std::optional<std::string> reasoning_effort;  // e.g. "low"/"medium"/"high" (OpenAI-style)
};

// Keeps the old SessionBackendConfig JSON format (type as string, api_key as string).
// This is the single serializer for BackendConfig: agents.json writes (AgentCrud,
// SaveAgentsConfig) and session persistence all reuse it so no field is dropped.
inline void to_json(nlohmann::json& j, const BackendConfig& cfg) {
  j = nlohmann::json{
    {"type", cfg.type == BackendType::kOpenAI ? "openai" : "ollama"},
    {"host", cfg.host},
    {"model", cfg.model},
    {"api_key", cfg.api_key.value_or("")},
    {"temperature", cfg.temperature},
    {"max_tokens", cfg.max_tokens},
    {"system_prompt", cfg.system_prompt.value_or("")},
    {"enable_thinking", cfg.enable_thinking},
  };
  if (cfg.reasoning_effort) j["reasoning_effort"] = *cfg.reasoning_effort;
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
  // Old session files may lack these fields; defaults keep them loadable.
  if (j.contains("system_prompt") && j["system_prompt"].is_string()) {
    auto sp = j["system_prompt"].get<std::string>();
    cfg.system_prompt = sp.empty() ? std::optional<std::string>{} : sp;
  } else {
    cfg.system_prompt = std::nullopt;
  }
  cfg.enable_thinking = j.value("enable_thinking", true);
  if (j.contains("reasoning_effort") && j["reasoning_effort"].is_string()) {
    auto re = j["reasoning_effort"].get<std::string>();
    cfg.reasoning_effort = re.empty() ? std::optional<std::string>{} : re;
  } else {
    cfg.reasoning_effort = std::nullopt;
  }
}

struct HistoryCompactionConfig {
  bool enabled = true;
  size_t keep_head = 10;
  size_t keep_tail = 50;
  std::string strategy = "truncate";  // reserved for future
};

inline void to_json(nlohmann::json& j, const HistoryCompactionConfig& cfg) {
  j = nlohmann::json{
      {"enabled", cfg.enabled},
      {"keep_head", cfg.keep_head},
      {"keep_tail", cfg.keep_tail},
      {"strategy", cfg.strategy},
  };
}

inline void from_json(const nlohmann::json& j, HistoryCompactionConfig& cfg) {
  cfg.enabled = j.value("enabled", true);
  cfg.keep_head = j.value("keep_head", 10);
  cfg.keep_tail = j.value("keep_tail", 50);
  cfg.strategy = j.value("strategy", "truncate");
}

struct AgentEntry {
  std::string name;
  std::string description;
  BackendConfig backend;
  std::vector<std::string> tools;
  SecurityPolicy security;
  std::vector<pu::mcp::McpServerConfig> mcp_servers;
  HistoryCompactionConfig compaction;
};

inline void to_json(nlohmann::json& j, const AgentEntry& entry) {
  j = nlohmann::json{
      {"name", entry.name},
      {"description", entry.description},
      {"tools", entry.tools},
      {"security", entry.security},
      {"backend", entry.backend},
      {"history_compaction", entry.compaction},
  };
  // Keep the on-disk format compact: omit mcp_servers when none are configured.
  if (!entry.mcp_servers.empty()) j["mcp_servers"] = entry.mcp_servers;
}

inline void from_json(const nlohmann::json& j, AgentEntry& entry) {
  entry.name = j.value("name", "");
  if (entry.name.empty()) throw pu::RuntimeError("AgentEntry missing field: name");
  entry.description = j.value("description", "");
  if (!j.contains("backend") || !j["backend"].is_object())
    throw pu::RuntimeError("AgentEntry missing field: backend");
  entry.backend = j["backend"].get<BackendConfig>();
  if (entry.backend.host.empty() || entry.backend.model.empty())
    throw pu::RuntimeError("AgentEntry missing field: backend.host or backend.model");

  entry.tools.clear();
  if (j.contains("tools") && j["tools"].is_array()) {
    for (const auto& t : j["tools"])
      if (t.is_string()) entry.tools.push_back(t.get<std::string>());
  }
  entry.security = SecurityPolicy{};
  if (j.contains("security") && j["security"].is_object())
    entry.security = j["security"].get<SecurityPolicy>();
  entry.mcp_servers.clear();
  if (j.contains("mcp_servers") && j["mcp_servers"].is_array()) {
    for (const auto& item : j["mcp_servers"]) {
      auto srv = item.get<pu::mcp::McpServerConfig>();
      if (!srv.name.empty() && !srv.command.empty())
        entry.mcp_servers.push_back(std::move(srv));
    }
  }
  // Defaults (enabled=true, keep_head=10, keep_tail=50, strategy="truncate")
  // are kept when history_compaction is absent.
  entry.compaction = HistoryCompactionConfig{};
  if (j.contains("history_compaction") && j["history_compaction"].is_object())
    entry.compaction = j["history_compaction"].get<HistoryCompactionConfig>();
}

struct AgentsConfig {
  std::string default_agent;
  std::vector<AgentEntry> agents;
};

std::filesystem::path FindConfigPath();
AgentsConfig LoadAgentsConfig(const std::string& config_path);
void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& config);

}  // namespace pu::config
