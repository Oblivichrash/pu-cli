// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "pu/model_config.hpp"

#include "backends/ollama/ollama_backend.hpp"
#include "backends/openai/openai_backend.hpp"
#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace pu::config {

using json = nlohmann::json;

namespace {

// Expand ${VAR} syntax with environment variable values.
// Prints warning to std::cerr if variable is not defined.
std::string ExpandEnvVars(const std::string& input) {
  static const std::regex env_re(R"(\$\{([^}]+)\})");
  std::string result = input;
  std::smatch match;
  while (std::regex_search(result, match, env_re)) {
    const char* env_val = std::getenv(match[1].str().c_str());
    std::string replacement = env_val ? env_val : "";
    if (!env_val) {
      std::cerr << "[WARN] Environment variable not set: " << match[1].str() << "\n";
    }
    result.replace(match.position(0), match.length(0), replacement);
  }
  return result;
}

BackendType ParseBackendType(const std::string& str) {
  if (str == "openai") return BackendType::kOpenAI;
  if (str == "ollama") return BackendType::kOllama;
  throw std::runtime_error("Unknown backend type: '" + str + "'");
}

BackendConfig ParseBackendConfig(const json& j) {
  BackendConfig cfg;
  cfg.type = ParseBackendType(j.value("type", "ollama"));
  cfg.host = ExpandEnvVars(j.value("host", ""));
  cfg.model = ExpandEnvVars(j.value("model", ""));
  if (j.contains("api_key")) {
    cfg.api_key = ExpandEnvVars(j["api_key"].get<std::string>());
  }
  cfg.temperature = j.value("temperature", 0.7f);
  if (j.contains("system_prompt")) {
    cfg.system_prompt = ExpandEnvVars(j["system_prompt"].get<std::string>());
  }
  return cfg;
}

}  // namespace

std::string FindConfigPath() {
  const char* env = std::getenv("PU_MODELS_CONFIG");
  if (env && env[0] != '\0') {
    return env;
  }

  if (std::filesystem::exists("./models.json")) {
    return "./models.json";
  }

  throw std::runtime_error("Configuration file not found. "
                           "Set PU_MODELS_CONFIG or place models.json in current directory.");
}

ModelsFile LoadModelsConfig(const std::string& config_path) {
  std::ifstream file(config_path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open config file: " + config_path);
  }

  json j;
  try {
    file >> j;
  } catch (const json::parse_error& e) {
    throw std::runtime_error("JSON parse error in " + config_path + ": " + e.what());
  }

  ModelsFile result;
  result.default_model = j.value("default_model", "");

  if (!j.contains("models") || !j["models"].is_array()) {
    throw std::runtime_error("Config file missing 'models' array");
  }

  for (const auto& item : j["models"]) {
    ModelEntry entry;
    entry.name = item.value("name", "");
    if (entry.name.empty()) {
      throw std::runtime_error("Model entry missing 'name'");
    }
    entry.description = item.value("description", "");
    if (!item.contains("backend") || !item["backend"].is_object()) {
      throw std::runtime_error("Model '" + entry.name + "' missing 'backend' block");
    }
    entry.backend = ParseBackendConfig(item["backend"]);
    if (entry.backend.host.empty()) {
      throw std::runtime_error("Model '" + entry.name + "': 'host' is required");
    }
    if (entry.backend.model.empty()) {
      throw std::runtime_error("Model '" + entry.name + "': 'model' is required");
    }
    result.models.push_back(std::move(entry));
  }

  if (result.default_model.empty() && !result.models.empty()) {
    result.default_model = result.models[0].name;
  }

  return result;
}

void SaveModelsConfig(const std::string& config_path, const ModelsFile& models) {
  json j;
  j["default_model"] = models.default_model;

  json models_array = json::array();
  for (const auto& entry : models.models) {
    json item;
    item["name"] = entry.name;
    item["description"] = entry.description;

    json backend;
    backend["type"] = (entry.backend.type == BackendType::kOpenAI) ? "openai" : "ollama";
    backend["host"] = entry.backend.host;
    backend["model"] = entry.backend.model;
    if (entry.backend.api_key) {
      backend["api_key"] = *entry.backend.api_key;
    }
    backend["temperature"] = entry.backend.temperature;
    if (entry.backend.system_prompt) {
      backend["system_prompt"] = *entry.backend.system_prompt;
    }
    item["backend"] = backend;

    models_array.push_back(item);
  }
  j["models"] = models_array;

  std::ofstream file(config_path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open config file for writing: " + config_path);
  }
  file << j.dump(2);  // pretty print with 2-space indent
}

std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg,
    std::unique_ptr<pu::http::HttpClient> http) {
  switch (cfg.type) {
    case BackendType::kOllama: {
      pu::backends::ollama::OllamaBackend::Config ollama_cfg;
      ollama_cfg.model = cfg.model;
      ollama_cfg.temperature = cfg.temperature;
      ollama_cfg.system_prompt = cfg.system_prompt;
      ollama_cfg.host = cfg.host;
      return std::make_unique<pu::backends::ollama::OllamaBackend>(
          std::move(ollama_cfg), std::move(http));
    }
    case BackendType::kOpenAI: {
      pu::backends::openai::OpenAIBackend::Config openai_cfg;
      openai_cfg.model = cfg.model;
      openai_cfg.temperature = cfg.temperature;
      openai_cfg.system_prompt = cfg.system_prompt;
      openai_cfg.host = cfg.host;
      openai_cfg.api_key = cfg.api_key.value_or("");
      return std::make_unique<pu::backends::openai::OpenAIBackend>(
          std::move(openai_cfg), std::move(http));
    }
    default:
      throw std::runtime_error("Unsupported backend type");
  }
}

}  // namespace pu::config
