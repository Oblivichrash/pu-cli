// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
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

// Supported backend types
enum class BackendType {
  kOllama,
  kOpenAI
};

// Backend-specific configuration block
struct BackendConfig {
  BackendType type = BackendType::kOllama;
  std::string host;
  std::string model;
  std::optional<std::string> api_key;      // only for OpenAI
  float temperature = 0.7f;
  std::optional<std::string> system_prompt;
};

// A single model entry
struct ModelEntry {
  std::string name;
  std::string description;
  BackendConfig backend;
};

// Top-level configuration file content
struct ModelsFile {
  std::string default_model;
  std::vector<ModelEntry> models;
};

// Find the configuration file path.
// Checks: PU_MODELS_CONFIG env var -> ./models.json
// Throws std::runtime_error if not found.
std::string FindConfigPath();

// Load and parse models.json from the given path.
// Throws std::runtime_error on file or parsing errors.
ModelsFile LoadModelsConfig(const std::string& config_path);

// Save the current configuration back to the file.
// Throws std::runtime_error on write errors.
void SaveModelsConfig(const std::string& config_path, const ModelsFile& models);

// Create a Backend instance from a BackendConfig.
// Requires HttpClient injection for network backends.
std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg,
    std::unique_ptr<pu::http::HttpClient> http);

}  // namespace pu::config
