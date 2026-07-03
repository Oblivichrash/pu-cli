// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_config.hpp"
#include "pu/agent_factory.hpp"
#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include "backends/ollama/ollama.hpp"
#include "backends/openai/openai.hpp"
#include "core/system.hpp"
#include "core/error.hpp"
#include "agents/llm/llm_agent.hpp"
#include "http/curl_http_client.hpp"
#include "pu/tools/execute_bash_tool.hpp"
#include "pu/tools/write_file_tool.hpp"
#include "pu/tools/create_tool.hpp"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <memory>
#include <system_error>

namespace pu::config {

using pu::ConfigErrc;

using json = nlohmann::json;

static std::string ExpandEnvVars(const std::string& input) {
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

static std::optional<BackendType> ParseBackendType(const std::string& s) noexcept {
  if (s == "openai") return BackendType::kOpenAI;
  if (s == "ollama") return BackendType::kOllama;
  return std::nullopt;
}

static ToolCallStyle ParseToolCallStyle(const std::string& str) {
  if (str == "openai") return ToolCallStyle::kOpenAI;
  if (str == "phi4") return ToolCallStyle::kPhi4;
  return ToolCallStyle::kDefault;
}

static SecurityPolicy ParseSecurityPolicy(const json& j) {
  SecurityPolicy policy;
  if (j.contains("sandbox_root") && j["sandbox_root"].is_string())
    policy.sandbox_root = j["sandbox_root"];
  if (j.contains("allowed_paths") && j["allowed_paths"].is_array()) {
    for (const auto& p : j["allowed_paths"])
      if (p.is_string()) policy.allowed_paths.push_back(p.get<std::string>());
  }
  if (j.contains("max_command_length") && j["max_command_length"].is_number())
    policy.max_command_length = j["max_command_length"];
  if (j.contains("forbidden_patterns") && j["forbidden_patterns"].is_array()) {
    for (const auto& pat : j["forbidden_patterns"])
      if (pat.is_string()) policy.forbidden_patterns.push_back(pat.get<std::string>());
  }
  return policy;
}

static BackendConfig ParseBackendConfig(const json& j, std::error_code& ec) {
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

static AgentEntry ParseAgentEntry(const json& j, std::error_code& ec) {
  AgentEntry entry;
  entry.name = j.value("name", "");
  if (entry.name.empty()) { ec = ConfigErrc::missing_field; return entry; }
  entry.description = j.value("description", "");
  if (!j.contains("backend") || !j["backend"].is_object()) { ec = ConfigErrc::missing_field; return entry; }
  entry.backend = ParseBackendConfig(j["backend"], ec);
  if (ec) return entry;
  if (entry.backend.host.empty() || entry.backend.model.empty()) { ec = ConfigErrc::missing_field; return entry; }

  if (j.contains("tools") && j["tools"].is_array()) {
    for (const auto& t : j["tools"])
      if (t.is_string()) entry.tools.push_back(t.get<std::string>());
  }
  if (j.contains("security") && j["security"].is_object())
    entry.security = ParseSecurityPolicy(j["security"]);

  return entry;
}

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
  result.default_agent = j["default_agent"];

  if (!j.contains("agents") || !j["agents"].is_array()) {
    ec = ConfigErrc::missing_field;
    return result;
  }

  for (const auto& item : j["agents"]) {
    std::error_code entry_ec;
    auto entry = ParseAgentEntry(item, entry_ec);
    if (entry_ec) { ec = entry_ec; return result; }
    result.agents.push_back(std::move(entry));
  }

  if (result.default_agent.empty() && !result.agents.empty())
    result.default_agent = result.agents[0].name;
  return result;
}

void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& config, std::error_code& ec) {
  ec.clear();
  json j;
  j["default_agent"] = config.default_agent;

  json agents_array = json::array();
  for (const auto& entry : config.agents) {
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
  if (!file.is_open()) { ec = ConfigErrc::file_not_found; return; }
  file << j.dump(2);
}

std::unique_ptr<pu::backend::Backend> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http, std::error_code& ec) {
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

namespace pu::agent {

AgentRegistry& AgentRegistry::Instance() {
  static AgentRegistry registry;
  return registry;
}

std::unique_ptr<BaseAgent> AgentRegistry::CreateAgent(const config::AgentEntry& entry) {
  auto http = std::make_unique<http::CurlHttpClient>();
  std::error_code ec;
  auto backend = config::CreateBackend(entry.backend, std::move(http), ec);
  if (ec) {
    throw std::runtime_error("Failed to create backend: " + ec.message());
  }

  auto tool_registry = std::make_unique<ToolRegistry>();

  for (const auto& tool_name : entry.tools) {
    if (tool_name == "execute_bash") {
      std::string sandbox = entry.security.sandbox_root.empty() ? "." : entry.security.sandbox_root;
      auto executor = std::make_unique<executor::CommandExecutor>(sandbox);
      tool_registry->RegisterTool(std::make_unique<tools::ExecuteBashToolStandard>(std::move(executor)));
    } else if (tool_name == "create_tool") {
      tool_registry->RegisterTool(std::make_unique<tools::CreateTool>());
    } else if (tool_name == "write_file") {
      tool_registry->RegisterTool(std::make_unique<tools::WriteFileTool>());
    }
  }

  return std::make_unique<agents::LLMAgent>(entry.name, std::move(backend), std::move(tool_registry), entry.security);
}

}  // namespace pu::agent
