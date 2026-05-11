// SPDX-License-Identifier: GPL-3.0-only
//
// Expert configuration loading and backend factory.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <system_error>

namespace pu::backend { class Backend; }
namespace pu::http { class HttpClient; }

namespace pu::config {

enum class BackendType {
  kOllama,
  kOpenAI
};

struct BackendConfig {
  BackendType type = BackendType::kOllama;
  std::string host;
  std::string model;
  std::optional<std::string> api_key;
  float temperature = 0.7f;
  std::optional<std::string> system_prompt;
};

enum class ExpertType {
  kChat,
  kBash
};

struct ExpertEntry {
  std::string name;
  ExpertType type = ExpertType::kChat;
  std::string description;
  BackendConfig backend;
  std::string sandbox_path = ".";
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
    std::error_code& ec);

}  // namespace pu::config
