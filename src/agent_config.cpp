// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_config.hpp"
#include "pu/error.hpp"
#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/openai_provider.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

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
    if (!env_val) spdlog::warn("Environment variable not set: {}", match[1].str());
    result.replace(match.position(0), match.length(0), replacement);
  }
  return result;
}

std::optional<BackendType> ParseBackendType(const std::string& s) noexcept {
  if (s == "openai") return BackendType::kOpenAI;
  if (s == "ollama") return BackendType::kOllama;
  return std::nullopt;
}

SecurityPolicy ParseSecurityPolicy(const json& j) {
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

BackendConfig ParseBackendConfig(const json& j) {
  BackendConfig cfg;
  auto type = ParseBackendType(j.value("type", "ollama"));
  if (!type) throw pu::Error("Unknown backend type");
  cfg.type = *type;
  cfg.host = ExpandEnvVars(j.value("host", ""));
  cfg.model = ExpandEnvVars(j.value("model", ""));
  if (j.contains("api_key")) cfg.api_key = ExpandEnvVars(j["api_key"].get<std::string>());
  cfg.temperature = j.value("temperature", 0.7f);
  if (j.contains("system_prompt")) cfg.system_prompt = ExpandEnvVars(j["system_prompt"].get<std::string>());
  cfg.parameters_as_string = j.value("parameters_as_string", false);
  cfg.max_tokens = j.value("max_tokens", 2048);
  cfg.enable_thinking = j.value("enable_thinking", true);
  return cfg;
}

std::vector<pu::mcp::McpServerConfig> ParseMcpServers(const json& j) {
  std::vector<pu::mcp::McpServerConfig> servers;
  if (!j.is_array()) return servers;
  for (const auto& item : j) {
    pu::mcp::McpServerConfig srv;
    srv.name = item.value("name", "");
    srv.command = item.value("command", "");
    if (item.contains("args") && item["args"].is_array()) {
      for (const auto& a : item["args"])
        if (a.is_string()) srv.args.push_back(a.get<std::string>());
    }
    if (!srv.name.empty() && !srv.command.empty())
      servers.push_back(std::move(srv));
    else
      spdlog::warn("Skipping MCP server entry with missing name or command");
  }
  return servers;
}

HistoryCompactionConfig ParseCompactionConfig(const json& j) {
  HistoryCompactionConfig cfg;
  if (j.contains("history_compaction") && j["history_compaction"].is_object()) {
    const auto& c = j["history_compaction"];
    cfg.enabled = c.value("enabled", true);
    cfg.keep_head = c.value("keep_head", 10);
    cfg.keep_tail = c.value("keep_tail", 50);
    cfg.strategy = c.value("strategy", "truncate");
  }
  return cfg;
}

AgentEntry ParseAgentEntry(const json& j) {
  AgentEntry entry;
  entry.name = j.value("name", "");
  if (entry.name.empty()) throw pu::Error("Missing agent name");
  entry.description = j.value("description", "");
  if (!j.contains("backend") || !j["backend"].is_object())
    throw pu::Error("Missing backend field");
  entry.backend = ParseBackendConfig(j["backend"]);
  if (entry.backend.host.empty() || entry.backend.model.empty())
    throw pu::Error("Missing host or model in backend");

  if (j.contains("tools") && j["tools"].is_array()) {
    for (const auto& t : j["tools"])
      if (t.is_string()) entry.tools.push_back(t.get<std::string>());
  }
  if (j.contains("security") && j["security"].is_object())
    entry.security = ParseSecurityPolicy(j["security"]);
  if (j.contains("mcp_servers") && j["mcp_servers"].is_array())
    entry.mcp_servers = ParseMcpServers(j["mcp_servers"]);

  entry.compaction = ParseCompactionConfig(j);
  return entry;
}

RuntimeLimits ParseRuntimeLimits(const json& j) {
  RuntimeLimits limits;
  if (j.contains("max_history_messages") && j["max_history_messages"].is_number())
    limits.max_history_messages = j["max_history_messages"];
  if (j.contains("max_branches") && j["max_branches"].is_number())
    limits.max_branches = j["max_branches"];
  if (j.contains("max_sessions") && j["max_sessions"].is_number())
    limits.max_sessions = j["max_sessions"];
  return limits;
}

} // unnamed namespace

std::string FindConfigPath() {
  if (auto* env = std::getenv("PU_AGENTS_CONFIG")) return env;
  if (std::filesystem::exists("./agents.json")) return "./agents.json";
  throw pu::Error("Configuration file not found. Set PU_AGENTS_CONFIG or place agents.json in current directory.");
}

AgentsConfig LoadAgentsConfig(const std::string& config_path) {
  AgentsConfig result;
  std::ifstream file(config_path);
  if (!file.is_open())
    throw pu::Error("Configuration file not found: " + config_path);

  json j;
  try { file >> j; } catch (const json::parse_error&) {
    throw pu::Error("Failed to parse configuration JSON");
  }

  if (!j.contains("default_agent") || !j["default_agent"].is_string())
    throw pu::Error("Missing default_agent field");
  result.default_agent = j["default_agent"];

  if (j.contains("limits") && j["limits"].is_object())
    result.limits = ParseRuntimeLimits(j["limits"]);

  if (!j.contains("agents") || !j["agents"].is_array())
    throw pu::Error("Missing agents array");
  for (const auto& item : j["agents"])
    result.agents.push_back(ParseAgentEntry(item));

  if (result.default_agent.empty() && !result.agents.empty())
    result.default_agent = result.agents[0].name;
  return result;
}

void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& cfg) {
  json j;
  j["default_agent"] = cfg.default_agent;
  json limits;
  limits["max_history_messages"] = cfg.limits.max_history_messages;
  limits["max_branches"] = cfg.limits.max_branches;
  limits["max_sessions"] = cfg.limits.max_sessions;
  j["limits"] = limits;

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
    backend["enable_thinking"] = entry.backend.enable_thinking;
    backend["tool_call_style"] = "default";
    item["backend"] = backend;

    if (!entry.mcp_servers.empty()) {
      json mcp_array = json::array();
      for (const auto& srv : entry.mcp_servers) {
        json srv_json;
        srv_json["name"] = srv.name;
        srv_json["command"] = srv.command;
        srv_json["args"] = srv.args;
        mcp_array.push_back(srv_json);
      }
      item["mcp_servers"] = mcp_array;
    }

    json compaction;
    compaction["enabled"] = entry.compaction.enabled;
    compaction["keep_head"] = entry.compaction.keep_head;
    compaction["keep_tail"] = entry.compaction.keep_tail;
    compaction["strategy"] = entry.compaction.strategy;
    item["history_compaction"] = compaction;

    agents_array.push_back(item);
  }
  j["agents"] = agents_array;

  std::ofstream file(config_path);
  if (!file.is_open()) throw pu::Error("Failed to open config file for writing: " + config_path);
  file << j.dump(2);
}

std::unique_ptr<pu::LLMProvider> CreateBackend(
    const BackendConfig& cfg, std::unique_ptr<pu::http::HttpClient> http) {
  switch (cfg.type) {
    case BackendType::kOllama: {
      OllamaProvider::Config ollama_cfg;
      ollama_cfg.model = cfg.model;
      ollama_cfg.temperature = cfg.temperature;
      ollama_cfg.system_prompt = cfg.system_prompt;
      ollama_cfg.host = cfg.host;
      ollama_cfg.api_key = cfg.api_key.value_or("");
      ollama_cfg.max_tokens = cfg.max_tokens;
      return std::make_unique<OllamaProvider>(std::move(ollama_cfg), std::move(http));
    }
    case BackendType::kOpenAI: {
      OpenAIProvider::Config openai_cfg;
      openai_cfg.model = cfg.model;
      openai_cfg.temperature = cfg.temperature;
      openai_cfg.system_prompt = cfg.system_prompt;
      openai_cfg.host = cfg.host;
      openai_cfg.api_key = cfg.api_key.value_or("");
      openai_cfg.parameters_as_string = cfg.parameters_as_string;
      openai_cfg.max_tokens = cfg.max_tokens;
      openai_cfg.enable_thinking = cfg.enable_thinking;
      return std::make_unique<OpenAIProvider>(openai_cfg, std::move(http));
    }
    default:
      throw pu::Error("Unknown backend type");
  }
}

} // namespace pu::config
