// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <system_error>

namespace pu::backend { class Backend; }
namespace pu::http { class HttpClient; }
namespace pu::backends { class ITokenAdapter; }

namespace pu::config {

enum class BackendType { kOllama, kOpenAI };
enum class AgentType { kLLM };
enum class ConfirmationPolicy { kAlwaysAsk, kAutoSafe, kNever };
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
};

struct AgentEntry {
  std::string name;
  AgentType type = AgentType::kLLM;
  std::string description;
  BackendConfig backend;
  std::string sandbox_path = ".";
  ConfirmationPolicy confirmation_policy = ConfirmationPolicy::kAlwaysAsk;
  std::vector<std::string> tools;
  std::unordered_map<std::string, std::string> tool_variants;
  SecurityPolicy security;
};

struct AgentsConfig {
  std::string default_expert;
  std::vector<AgentEntry> experts;
};

std::string FindConfigPath();
AgentsConfig LoadAgentsConfig(const std::string& config_path, std::error_code& ec);
void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& config,
                      std::error_code& ec);
std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http,
    std::unique_ptr<pu::backends::ITokenAdapter> adapter, std::error_code& ec);

}  // namespace pu::config
