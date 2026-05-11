// SPDX-License-Identifier: GPL-3.0-only

#include "pu/expert_config.hpp"

#include "backends/ollama/ollama_backend.hpp"
#include "backends/openai/openai_backend.hpp"
#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include "pu/error_codes.hpp"

#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace pu::config {

using json = nlohmann::json;

namespace {

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

ExpertType ParseExpertType(const std::string& str) {
  if (str == "chat") return ExpertType::kChat;
  if (str == "bash") return ExpertType::kBash;
  throw std::runtime_error("Unknown expert type: '" + str + "'");
}

BackendConfig ParseBackendConfig(const json& j, std::error_code& ec) {
  BackendConfig cfg;
  try {
    cfg.type = ParseBackendType(j.value("type", "ollama"));
  } catch (const std::exception&) {
    ec = ConfigErrc::backend_unknown;
    return cfg;
  }
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

ExpertEntry ParseExpertEntry(const json& j, std::error_code& ec) {
  ExpertEntry entry;
  entry.name = j.value("name", "");
  if (entry.name.empty()) {
    ec = ConfigErrc::missing_field;
    return entry;
  }
  entry.description = j.value("description", "");
  try {
    entry.type = ParseExpertType(j.value("type", "chat"));
  } catch (const std::exception&) {
    ec = ConfigErrc::missing_field;
    return entry;
  }
  if (!j.contains("backend") || !j["backend"].is_object()) {
    ec = ConfigErrc::missing_field;
    return entry;
  }
  entry.backend = ParseBackendConfig(j["backend"], ec);
  if (ec) return entry;
  if (entry.backend.host.empty()) {
    ec = ConfigErrc::missing_field;
    return entry;
  }
  if (entry.backend.model.empty()) {
    ec = ConfigErrc::missing_field;
    return entry;
  }
  if (entry.type == ExpertType::kBash && j.contains("executor") && j["executor"].is_object()) {
    entry.sandbox_path = j["executor"].value("sandbox", ".");
  }
  return entry;
}

}  // namespace

std::string FindConfigPath() {
  const char* env = std::getenv("PU_EXPERTS_CONFIG");
  if (env && env[0] != '\0') {
    return env;
  }
  if (std::filesystem::exists("./experts.json")) {
    return "./experts.json";
  }
  throw std::runtime_error("Configuration file not found. "
                           "Set PU_EXPERTS_CONFIG or place experts.json in current directory.");
}

ExpertsConfig LoadExpertsConfig(const std::string& config_path, std::error_code& ec) {
  ec.clear();
  ExpertsConfig result;

  std::ifstream file(config_path);
  if (!file.is_open()) {
    ec = ConfigErrc::file_not_found;
    return result;
  }

  json j;
  try {
    file >> j;
  } catch (const json::parse_error& e) {
    ec = ConfigErrc::parse_error;
    return result;
  }

  result.default_expert = j.value("default_expert", "");
  if (!j.contains("experts") || !j["experts"].is_array()) {
    ec = ConfigErrc::missing_field;
    return result;
  }

  for (const auto& item : j["experts"]) {
    std::error_code entry_ec;
    auto entry = ParseExpertEntry(item, entry_ec);
    if (entry_ec) {
      ec = entry_ec;
      return result;
    }
    result.experts.push_back(std::move(entry));
  }

  if (result.default_expert.empty() && !result.experts.empty()) {
    result.default_expert = result.experts[0].name;
  }
  return result;
}

void SaveExpertsConfig(const std::string& config_path,
                       const ExpertsConfig& config,
                       std::error_code& ec) {
  ec.clear();
  json j;
  j["default_expert"] = config.default_expert;
  json experts_array = json::array();
  for (const auto& entry : config.experts) {
    json item;
    item["name"] = entry.name;
    item["description"] = entry.description;
    item["type"] = (entry.type == ExpertType::kChat) ? "chat" : "bash";
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
    if (entry.type == ExpertType::kBash) {
      item["executor"] = {{"sandbox", entry.sandbox_path}};
    }
    experts_array.push_back(item);
  }
  j["experts"] = experts_array;

  std::ofstream file(config_path);
  if (!file.is_open()) {
    ec = ConfigErrc::file_not_found;
    return;
  }
  file << j.dump(2);
}

std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg,
    std::unique_ptr<pu::http::HttpClient> http,
    std::error_code& ec) {
  ec.clear();
  switch (cfg.type) {
    case BackendType::kOllama: {
      pu::backends::ollama::OllamaBackend::Config ollama_cfg;
      ollama_cfg.model = cfg.model;
      ollama_cfg.temperature = cfg.temperature;
      ollama_cfg.system_prompt = cfg.system_prompt;
      ollama_cfg.host = cfg.host;
      ollama_cfg.api_key = cfg.api_key.value_or("");
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
      ec = ConfigErrc::backend_unknown;
      return nullptr;
  }
}

}  // namespace pu::config
