// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <system_error>

namespace pu::backend { class Backend; }
namespace pu::http { class HttpClient; }
namespace pu::backends { class ITokenAdapter; }

namespace pu::config {

enum class BackendType { kOllama, kOpenAI };
enum class AgentType { kChat, kBash };
enum class ConfirmationPolicy { kAlwaysAsk, kAutoSafe, kNever };
enum class ToolCallStyle { kDefault, kOpenAI, kPhi4 };

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
  AgentType type = AgentType::kChat;
  std::string description;
  BackendConfig backend;
  std::string sandbox_path = ".";
  ConfirmationPolicy confirmation_policy = ConfirmationPolicy::kAlwaysAsk;
};

struct AgentsConfig {
  std::string default_expert;
  std::vector<AgentEntry> experts;
};

std::string FindConfigPath();
AgentsConfig LoadExpertsConfig(const std::string& config_path, std::error_code& ec);
void SaveExpertsConfig(const std::string& config_path, const AgentsConfig& config,
                       std::error_code& ec);
std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http,
    std::unique_ptr<pu::backends::ITokenAdapter> adapter, std::error_code& ec);

}  // namespace pu::config
