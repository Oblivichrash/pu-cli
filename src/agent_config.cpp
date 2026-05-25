// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_config.hpp"

#include "backends/ollama/ollama_backend.hpp"
#include "backends/openai/openai_backend.hpp"
#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include "pu/error_codes.hpp"
#include "pu/token_adapter.hpp"

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
    if (!env_val) std::cerr << "[WARN] Environment variable not set: " << match[1].str() << "\n";
    result.replace(match.position(0), match.length(0), replacement);
  }
  return result;
}

std::optional<BackendType> ParseBackendType(const std::string& s) noexcept {
  if (s == "openai") return BackendType::kOpenAI;
  if (s == "ollama") return BackendType::kOllama;
  return std::nullopt;
}

std::optional<AgentType> ParseAgentType(const std::string& s) noexcept {
  if (s == "chat") return AgentType::kChat;
  if (s == "bash") return AgentType::kBash;
  return std::nullopt;
}

ConfirmationPolicy ParseConfirmationPolicy(const std::string& str) {
  if (str == "auto_safe") return ConfirmationPolicy::kAutoSafe;
  if (str == "never") return ConfirmationPolicy::kNever;
  return ConfirmationPolicy::kAlwaysAsk;
}

ToolCallStyle ParseToolCallStyle(const std::string& str) {
  if (str == "openai") return ToolCallStyle::kOpenAI;
  if (str == "phi4") return ToolCallStyle::kPhi4;
  return ToolCallStyle::kDefault;
}

BackendConfig ParseBackendConfig(const json& j, std::error_code& ec) {
  BackendConfig cfg;
  auto type = ParseBackendType(j.value("type", "ollama"));
  if (!type) { ec = ConfigErrc::backend_unknown; return cfg; }
  cfg.type = *type;
  cfg.host = ExpandEnvVars(j.value("host", ""));
  cfg.model = ExpandEnvVars(j.value("model", ""));
  if (j.contains("api_key")) cfg.api_key = ExpandEnvVars(j["api_key"].get<std::string>());
  cfg.temperature = j.value("temperature", 0.7f);
  if (j.contains("system_prompt")) cfg.system_prompt = ExpandEnvVars(j["system_prompt"].get<std::string>());
  cfg.tool_call_style = ParseToolCallStyle(j.value("tool_call_style", "default"));
  return cfg;
}

AgentEntry ParseAgentEntry(const json& j, std::error_code& ec) {
  AgentEntry entry;
  entry.name = j.value("name", "");
  if (entry.name.empty()) { ec = ConfigErrc::missing_field; return entry; }
  entry.description = j.value("description", "");
  auto atype = ParseAgentType(j.value("type", "chat"));
  if (!atype) { ec = ConfigErrc::missing_field; return entry; }
  entry.type = *atype;
  if (!j.contains("backend") || !j["backend"].is_object()) { ec = ConfigErrc::missing_field; return entry; }
  entry.backend = ParseBackendConfig(j["backend"], ec);
  if (ec) return entry;
  if (entry.backend.host.empty() || entry.backend.model.empty()) { ec = ConfigErrc::missing_field; return entry; }
  if (entry.type == AgentType::kBash && j.contains("executor") && j["executor"].is_object()) {
    entry.sandbox_path = j["executor"].value("sandbox", ".");
    entry.confirmation_policy = ParseConfirmationPolicy(j["executor"].value("confirmation", "always"));
  }
  return entry;
}

}  // namespace

std::string FindConfigPath() {
  if (auto* env = std::getenv("PU_AGENTS_CONFIG")) return env;
  if (std::filesystem::exists("./agents.json")) return "./agents.json";
  throw std::runtime_error("Configuration file not found. "
                           "Set PU_AGENTS_CONFIG or place agents.json in current directory.");
}

AgentsConfig LoadAgentsConfig(const std::string& config_path, std::error_code& ec) {
  ec.clear();
  AgentsConfig result;
  std::ifstream file(config_path);
  if (!file.is_open()) { ec = ConfigErrc::file_not_found; return result; }

  json j;
  try { file >> j; } catch (const json::parse_error&) { ec = ConfigErrc::parse_error; return result; }

  if (!j.contains("default_agent") || !j["default_agent"].is_string()) {
    ec = ConfigErrc::missing_field;
    return result;
  }
  result.default_expert = j["default_agent"];

  if (!j.contains("agents") || !j["agents"].is_array()) {
    ec = ConfigErrc::missing_field;
    return result;
  }

  for (const auto& item : j["agents"]) {
    std::error_code entry_ec;
    auto entry = ParseAgentEntry(item, entry_ec);
    if (entry_ec) { ec = entry_ec; return result; }
    result.experts.push_back(std::move(entry));
  }

  if (result.default_expert.empty() && !result.experts.empty()) {
    result.default_expert = result.experts[0].name;
  }
  return result;
}

void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& config,
                      std::error_code& ec) {
  ec.clear();
  json j;
  j["default_agent"] = config.default_expert;

  json agents_array = json::array();
  for (const auto& entry : config.experts) {
    json item;
    item["name"] = entry.name;
    item["description"] = entry.description;
    item["type"] = (entry.type == AgentType::kChat) ? "chat" : "bash";

    json backend;
    backend["type"] = (entry.backend.type == BackendType::kOpenAI) ? "openai" : "ollama";
    backend["host"] = entry.backend.host;
    backend["model"] = entry.backend.model;
    if (entry.backend.api_key) backend["api_key"] = *entry.backend.api_key;
    backend["temperature"] = entry.backend.temperature;
    if (entry.backend.system_prompt) backend["system_prompt"] = *entry.backend.system_prompt;
    switch (entry.backend.tool_call_style) {
      case ToolCallStyle::kOpenAI: backend["tool_call_style"] = "openai"; break;
      case ToolCallStyle::kPhi4:   backend["tool_call_style"] = "phi4"; break;
      default: backend["tool_call_style"] = "default";
    }
    item["backend"] = backend;

    if (entry.type == AgentType::kBash) {
      json executor = {{"sandbox", entry.sandbox_path}};
      switch (entry.confirmation_policy) {
        case ConfirmationPolicy::kAutoSafe: executor["confirmation"] = "auto_safe"; break;
        case ConfirmationPolicy::kNever:    executor["confirmation"] = "never"; break;
        default: executor["confirmation"] = "always";
      }
      item["executor"] = executor;
    }
    agents_array.push_back(item);
  }
  j["agents"] = agents_array;

  std::ofstream file(config_path);
  if (!file.is_open()) { ec = ConfigErrc::file_not_found; return; }
  file << j.dump(2);
}

std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http,
    std::unique_ptr<pu::backends::ITokenAdapter> adapter, std::error_code& ec) {
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
          std::move(ollama_cfg), std::move(http), std::move(adapter));
    }
    case BackendType::kOpenAI: {
      pu::backends::openai::OpenAIBackend::Config openai_cfg;
      openai_cfg.model = cfg.model;
      openai_cfg.temperature = cfg.temperature;
      openai_cfg.system_prompt = cfg.system_prompt;
      openai_cfg.host = cfg.host;
      openai_cfg.api_key = cfg.api_key.value_or("");
      return std::make_unique<pu::backends::openai::OpenAIBackend>(
          std::move(openai_cfg), std::move(http), std::move(adapter));
    }
    default:
      ec = ConfigErrc::backend_unknown;
      return nullptr;
  }
}

}  // namespace pu::config
