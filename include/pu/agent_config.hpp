// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pu/llm/llm_provider.hpp"
#include "pu/http/http_client.hpp"
#include "pu/mcp/mcp_types.hpp"

namespace pu::config {

enum class BackendType { kOllama, kOpenAI };
enum class ToolCallStyle { kDefault, kOpenAI, kPhi4 };

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
  ToolCallStyle tool_call_style = ToolCallStyle::kDefault;
  bool parameters_as_string = false;
  int max_tokens = 2048;
  bool enable_thinking = true;  // for DeepSeek/vLLM only
};

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

struct RuntimeLimits {
  size_t max_history_messages = 10000;
  size_t max_branches = 20;
  size_t max_sessions = 10;
};

struct AgentsConfig {
  std::string default_agent;
  std::vector<AgentEntry> agents;
  RuntimeLimits limits;
};

std::string FindConfigPath();
AgentsConfig LoadAgentsConfig(const std::string& config_path);
void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& config);
std::unique_ptr<pu::LLMProvider> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http);

}  // namespace pu::config
