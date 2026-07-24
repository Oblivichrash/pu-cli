// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_core.hpp"

#include "backends/ollama/ollama_backend.hpp"
#include "backends/openai/openai_backend.hpp"
#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace pu::agent::config {

using json = nlohmann::json;

namespace {

std::string ExpandEnvVars(const std::string& input) {
  static const std::regex env_re(R"(\$\{([^}]+)\})");
  std::string result = input;
  std::smatch match;
  while (std::regex_search(result, match, env_re)) {
    const char* env_val = std::getenv(match[1].str().c_str());
    std::string replacement = env_val ? env_val : "";
    if (!env_val) std::cerr << "[WARN] Environment variable not set: " << match[1].str() << '\n';
    result.replace(match.position(0), match.length(0), replacement);
  }
  return result;
}

std::optional<BackendType> ParseBackendType(const std::string& s) noexcept {
  if (s == "openai") return BackendType::kOpenAI;
  if (s == "ollama") return BackendType::kOllama;
  return std::nullopt;
}

ToolCallStyle ParseToolCallStyle(const std::string& str) {
  if (str == "openai") return ToolCallStyle::kOpenAI;
  if (str == "phi4") return ToolCallStyle::kPhi4;
  return ToolCallStyle::kDefault;
}

SecurityPolicy ParseSecurityPolicy(const json& j) {
  SecurityPolicy policy;
  if (j.contains("sandbox_root") && j["sandbox_root"].is_string()) {
    policy.sandbox_root = j["sandbox_root"];
  }
  if (j.contains("allowed_paths") && j["allowed_paths"].is_array()) {
    for (const auto& p : j["allowed_paths"]) {
      if (p.is_string()) policy.allowed_paths.push_back(p.get<std::string>());
    }
  }
  if (j.contains("max_command_length") && j["max_command_length"].is_number()) {
    policy.max_command_length = j["max_command_length"];
  }
  if (j.contains("forbidden_patterns") && j["forbidden_patterns"].is_array()) {
    for (const auto& pat : j["forbidden_patterns"]) {
      if (pat.is_string()) policy.forbidden_patterns.push_back(pat.get<std::string>());
    }
  }
  return policy;
}

BackendConfig ParseBackendConfig(const json& j) {
  BackendConfig cfg;
  auto type = ParseBackendType(j.value("type", "ollama"));
  if (!type) throw std::runtime_error("Unknown backend type");
  cfg.type = *type;
  cfg.host = ExpandEnvVars(j.value("host", ""));
  cfg.model = ExpandEnvVars(j.value("model", ""));
  if (j.contains("api_key")) cfg.api_key = ExpandEnvVars(j["api_key"].get<std::string>());
  cfg.temperature = j.value("temperature", 0.7f);
  if (j.contains("system_prompt")) cfg.system_prompt = ExpandEnvVars(j["system_prompt"].get<std::string>());
  cfg.tool_call_style = ParseToolCallStyle(j.value("tool_call_style", "default"));
  cfg.parameters_as_string = j.value("parameters_as_string", false);
  cfg.max_tokens = j.value("max_tokens", 2048);
  return cfg;
}

AgentEntry ParseAgentEntry(const json& j) {
  AgentEntry entry;
  entry.name = j.value("name", "");
  if (entry.name.empty()) throw std::runtime_error("Missing agent name field");
  entry.description = j.value("description", "");
  if (!j.contains("backend") || !j["backend"].is_object()) { throw std::runtime_error("Missing backend field"); }
  entry.backend = ParseBackendConfig(j["backend"]);
  if (entry.backend.host.empty() || entry.backend.model.empty()) { throw std::runtime_error("Missing host or model in backend config"); }

  if (j.contains("tools") && j["tools"].is_array()) {
    for (const auto& t : j["tools"]) {
      if (t.is_string()) entry.tools.push_back(t.get<std::string>());
    }
  }
  if (j.contains("security") && j["security"].is_object()) {
    entry.security = ParseSecurityPolicy(j["security"]);
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

AgentsConfig LoadAgentsConfig(const std::string& config_path) {
  AgentsConfig result;
  std::ifstream file(config_path);
  if (!file.is_open()) { throw std::runtime_error("Configuration file not found: " + config_path); }

  json j;
  try { file >> j; } catch (const json::parse_error&) { throw std::runtime_error("Failed to parse configuration JSON"); }

  if (!j.contains("default_agent") || !j["default_agent"].is_string()) {
    throw std::runtime_error("Missing default_agent field in config");
  }
  result.default_agent = j["default_agent"];

  if (!j.contains("agents") || !j["agents"].is_array()) {
    throw std::runtime_error("Missing agents array in config");
  }

  for (const auto& item : j["agents"]) {
    auto entry = ParseAgentEntry(item);
    result.agents.push_back(std::move(entry));
  }

  if (result.default_agent.empty() && !result.agents.empty()) {
    result.default_agent = result.agents[0].name;
  }
  return result;
}

void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& cfg) {
  json j;
  j["default_agent"] = cfg.default_agent;

  json agents_array = json::array();
  for (const auto& entry : cfg.agents) {
    json item;
    item["name"] = entry.name;
    item["description"] = entry.description;
    item["tools"] = entry.tools;

    json security;
    security["sandbox_root"] = entry.security.sandbox_root;
    security["allowed_paths"] = entry.security.allowed_paths;
    security["max_command_length"] = entry.security.max_command_length;
    security["forbidden_patterns"] = entry.security.forbidden_patterns;
    item["security"] = security;

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
    agents_array.push_back(item);
  }
  j["agents"] = agents_array;

  std::ofstream file(config_path);
  if (!file.is_open()) { throw std::runtime_error("Failed to open config file for writing: " + config_path); }
  file << j.dump(2);
}

std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http) {
  switch (cfg.type) {
    case BackendType::kOllama: {
      pu::backends::ollama::OllamaBackend::Config ollama_cfg;
      ollama_cfg.model = cfg.model;
      ollama_cfg.temperature = cfg.temperature;
      ollama_cfg.system_prompt = cfg.system_prompt;
      ollama_cfg.host = cfg.host;
      ollama_cfg.api_key = cfg.api_key.value_or("");
      ollama_cfg.max_tokens = cfg.max_tokens;
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
      openai_cfg.parameters_as_string = cfg.parameters_as_string;
      openai_cfg.max_tokens = cfg.max_tokens;
      return std::make_unique<pu::backends::openai::OpenAIBackend>(
          openai_cfg, std::move(http));
    }
    default:
      throw std::runtime_error("Unknown backend type");
  }
}

}  // namespace pu::agent::config
