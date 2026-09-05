// SPDX-License-Identifier: GPL-3.0-only
#include "pu/agent_config.hpp"
#include "pu/error.hpp"
#include "pu/llm/ollama_provider.hpp"
#include "pu/llm/openai_provider.hpp"
#include "pu/json.hpp"
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace pu::config {

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

SecurityPolicy ParseSecurityPolicy(const json::value& j) {
  SecurityPolicy policy;
  if (json::HasKey(j, "sandbox_root") && j.at("sandbox_root").is_string())
    policy.sandbox_root = boost::json::value_to<std::string>(j.at("sandbox_root"));
  if (json::HasKey(j, "allowed_paths") && j.at("allowed_paths").is_array()) {
    for (const auto& p : j.at("allowed_paths").as_array())
      if (p.is_string()) policy.allowed_paths.push_back(boost::json::value_to<std::string>(p));
  }
  if (json::HasKey(j, "max_command_length") && j.at("max_command_length").is_number())
    policy.max_command_length =
        boost::json::value_to<std::size_t>(j.at("max_command_length"));
  if (json::HasKey(j, "forbidden_patterns") && j.at("forbidden_patterns").is_array()) {
    for (const auto& pat : j.at("forbidden_patterns").as_array())
      if (pat.is_string()) policy.forbidden_patterns.push_back(boost::json::value_to<std::string>(pat));
  }
  return policy;
}

BackendConfig ParseBackendConfig(const json::value& j) {
  BackendConfig cfg;
  auto type = ParseBackendType(json::ValueOrDefault<std::string>(j, "type", "ollama"));
  if (!type) throw pu::Error("Unknown backend type");
  cfg.type = *type;
  cfg.host = ExpandEnvVars(json::ValueOrDefault<std::string>(j, "host", ""));
  cfg.model = ExpandEnvVars(json::ValueOrDefault<std::string>(j, "model", ""));
  if (json::HasKey(j, "api_key"))
    cfg.api_key = ExpandEnvVars(boost::json::value_to<std::string>(j.at("api_key")));
  cfg.temperature = json::ValueOrDefault<float>(j, "temperature", 0.7f);
  if (json::HasKey(j, "system_prompt"))
    cfg.system_prompt = ExpandEnvVars(boost::json::value_to<std::string>(j.at("system_prompt")));
  cfg.parameters_as_string = json::ValueOrDefault<bool>(j, "parameters_as_string", false);
  cfg.max_tokens = json::ValueOrDefault<int>(j, "max_tokens", 2048);
  cfg.enable_thinking = json::ValueOrDefault<bool>(j, "enable_thinking", true);
  return cfg;
}

std::vector<pu::mcp::McpServerConfig> ParseMcpServers(const json::value& j) {
  std::vector<pu::mcp::McpServerConfig> servers;
  if (!j.is_array()) return servers;
  for (const auto& item : j.as_array()) {
    pu::mcp::McpServerConfig srv;
    srv.name = json::ValueOrDefault<std::string>(item, "name", "");
    srv.command = json::ValueOrDefault<std::string>(item, "command", "");
    if (json::HasKey(item, "args") && item.at("args").is_array()) {
      for (const auto& a : item.at("args").as_array())
        if (a.is_string()) srv.args.push_back(boost::json::value_to<std::string>(a));
    }
    // Remote HTTP MCP endpoint (streamable HTTP). When present, McpClient
    // connects over HTTP instead of spawning the stdio command.
    if (json::HasKey(item, "url") && item.at("url").is_string())
      srv.url = ExpandEnvVars(boost::json::value_to<std::string>(item.at("url")));
    if (json::HasKey(item, "headers") && item.at("headers").is_object()) {
      for (const auto& kv : item.at("headers").as_object()) {
        if (kv.value().is_string())
          srv.headers[std::string(kv.key())] =
              ExpandEnvVars(boost::json::value_to<std::string>(kv.value()));
      }
    }

    if (!srv.name.empty() && (!srv.command.empty() || !srv.url.empty()))
      servers.push_back(std::move(srv));
    else
      spdlog::warn("Skipping MCP server entry with missing name, command, or url");
  }
  return servers;
}

HistoryCompactionConfig ParseCompactionConfig(const json::value& j) {
  HistoryCompactionConfig cfg;
  if (json::HasKey(j, "history_compaction") && j.at("history_compaction").is_object()) {
    const auto& c = j.at("history_compaction");
    cfg.enabled = json::ValueOrDefault<bool>(c, "enabled", true);
    cfg.keep_head = json::ValueOrDefault<std::size_t>(c, "keep_head", 10);
    cfg.keep_tail = json::ValueOrDefault<std::size_t>(c, "keep_tail", 50);
  }
  return cfg;
}

AgentEntry ParseAgentEntry(const json::value& j) {
  AgentEntry entry;
  entry.name = json::ValueOrDefault<std::string>(j, "name", "");
  if (entry.name.empty()) throw pu::Error("Missing agent name");
  entry.description = json::ValueOrDefault<std::string>(j, "description", "");
  if (!json::HasKey(j, "backend") || !j.at("backend").is_object())
    throw pu::Error("Missing backend field");
  entry.backend = ParseBackendConfig(j.at("backend"));
  if (entry.backend.host.empty() || entry.backend.model.empty())
    throw pu::Error("Missing host or model in backend");

  if (json::HasKey(j, "tools") && j.at("tools").is_array()) {
    for (const auto& t : j.at("tools").as_array())
      if (t.is_string()) entry.tools.push_back(boost::json::value_to<std::string>(t));
  }
  if (json::HasKey(j, "security") && j.at("security").is_object())
    entry.security = ParseSecurityPolicy(j.at("security"));
  if (json::HasKey(j, "mcp_servers") && j.at("mcp_servers").is_array())
    entry.mcp_servers = ParseMcpServers(j.at("mcp_servers"));

  entry.compaction = ParseCompactionConfig(j);
  return entry;
}

} // unnamed namespace

std::string FindConfigPath() {
  const std::filesystem::path project = "./.pu/agents.json";
  if (std::filesystem::exists(project)) return project.string();

  const char* home = std::getenv("HOME");
  if (home) {
    const std::filesystem::path user =
        std::filesystem::path(home) / ".pu" / "agents.json";
    if (std::filesystem::exists(user)) return user.string();
  }

  throw pu::Error(
      "Configuration file not found. Place agents.json in ./.pu/ or ~/.pu/.");
}

AgentsConfig LoadAgentsConfig(const std::string& config_path) {
  AgentsConfig result;
  std::ifstream file(config_path);
  if (!file.is_open())
    throw pu::Error("Configuration file not found: " + config_path);

  json::value j;
  try {
    std::ostringstream buffer;
    buffer << file.rdbuf();
    j = json::parse(buffer.str());
  } catch (const boost::system::system_error&) {
    throw pu::Error("Failed to parse configuration JSON");
  }

  if (!json::HasKey(j, "default_agent") || !j.at("default_agent").is_string())
    throw pu::Error("Missing default_agent field");
  result.default_agent = boost::json::value_to<std::string>(j.at("default_agent"));

  if (!json::HasKey(j, "agents") || !j.at("agents").is_array())
    throw pu::Error("Missing agents array");
  for (const auto& item : j.at("agents").as_array())
    result.agents.push_back(ParseAgentEntry(item));

  if (result.default_agent.empty() && !result.agents.empty())
    result.default_agent = result.agents[0].name;
  return result;
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
