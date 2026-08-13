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

} // unnamed namespace

std::string FindConfigPath() {
  if (auto* env = std::getenv("PU_AGENTS_CONFIG")) return env;
  if (std::filesystem::exists("./agents.json")) return "./agents.json";
  if (const char* home = std::getenv("HOME")) {
    auto user_cfg = std::filesystem::path(home) / ".pu" / "agents.json";
    if (std::filesystem::exists(user_cfg)) return user_cfg.string();
  }
  throw pu::Error("Configuration file not found. Set PU_AGENTS_CONFIG or place "
                  "agents.json in the current directory or ~/.pu/.");
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

  if (!j.contains("agents") || !j["agents"].is_array())
    throw pu::Error("Missing agents array");
  for (const auto& item : j["agents"]) {
    // from_json parses every AgentEntry field (backend, tools, security,
    // mcp_servers, history_compaction) using the shared serializers above.
    AgentEntry entry = item.get<AgentEntry>();
    // Env-var expansion happens after parsing so the header serializers stay
    // environment-free (raw values are preserved in to_json round-trips).
    entry.backend.host = ExpandEnvVars(entry.backend.host);
    entry.backend.model = ExpandEnvVars(entry.backend.model);
    if (entry.backend.api_key) entry.backend.api_key = ExpandEnvVars(*entry.backend.api_key);
    if (entry.backend.system_prompt) entry.backend.system_prompt = ExpandEnvVars(*entry.backend.system_prompt);
    // A ${VAR} that expands to empty must still fail validation, as before.
    if (entry.backend.host.empty() || entry.backend.model.empty())
      throw pu::Error("Missing host or model in backend");
    result.agents.push_back(std::move(entry));
  }

  if (result.default_agent.empty() && !result.agents.empty())
    result.default_agent = result.agents[0].name;
  return result;
}

void SaveAgentsConfig(const std::string& config_path, const AgentsConfig& cfg) {
  json j;
  j["default_agent"] = cfg.default_agent;
  json agents_array = json::array();
  for (const auto& entry : cfg.agents)
    agents_array.push_back(entry);  // AgentEntry::to_json serializes all fields
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
        openai_cfg.max_tokens = cfg.max_tokens;
      openai_cfg.enable_thinking = cfg.enable_thinking;
      openai_cfg.reasoning_effort = cfg.reasoning_effort;
      return std::make_unique<OpenAIProvider>(openai_cfg, std::move(http));
    }
    default:
      throw pu::Error("Unknown backend type");
  }
}

} // namespace pu::config
