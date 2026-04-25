// SPDX-License-Identifier: GPL-3.0-only
//
// Model configuration loading and backend factory.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pu::backend {
class Backend;
}  // namespace pu::backend

namespace pu::http {
class HttpClient;
}  // namespace pu::http

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

struct ModelEntry {
  std::string name;
  std::string description;
  BackendConfig backend;
};

struct ModelsFile {
  std::string default_model;
  std::vector<ModelEntry> models;
};

std::string FindConfigPath();
ModelsFile LoadModelsConfig(const std::string& config_path);
void SaveModelsConfig(const std::string& config_path, const ModelsFile& models);

std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg,
    std::unique_ptr<pu::http::HttpClient> http);

}  // namespace pu::config
