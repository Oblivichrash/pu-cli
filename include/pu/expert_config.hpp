// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include "pu/token_adapter.hpp"
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
enum class ExpertType { kChat, kBash };
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

struct ExpertEntry {
  std::string name;
  ExpertType type = ExpertType::kChat;
  std::string description;
  BackendConfig backend;
  std::string sandbox_path = ".";
  ConfirmationPolicy confirmation_policy = ConfirmationPolicy::kAlwaysAsk;
};

struct ExpertsConfig {
  std::string default_expert;
  std::vector<ExpertEntry> experts;
};

std::string FindConfigPath();
ExpertsConfig LoadExpertsConfig(const std::string& config_path, std::error_code& ec);
void SaveExpertsConfig(const std::string& config_path, const ExpertsConfig& config, std::error_code& ec);
std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg,
    std::unique_ptr<pu::http::HttpClient> http,
    std::unique_ptr<pu::backends::ITokenAdapter> adapter,
    std::error_code& ec);

}  // namespace pu::config
