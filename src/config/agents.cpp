// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/agents.hpp"

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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

SecurityPolicy ParseSecurityPolicy(const json::value& j) {
  SecurityPolicy policy;
  if (json::HasKey(j, "sandbox_root") && j.at("sandbox_root").is_string())
    policy.sandbox_root = boost::json::value_to<std::string>(j.at("sandbox_root"));
  if (json::HasKey(j, "max_command_length") && j.at("max_command_length").is_number())
    policy.max_command_length = boost::json::value_to<std::size_t>(j.at("max_command_length"));
  if (json::HasKey(j, "forbidden_patterns") && j.at("forbidden_patterns").is_array()) {
    for (const auto& pat : j.at("forbidden_patterns").as_array())
      if (pat.is_string())
        policy.forbidden_patterns.push_back(boost::json::value_to<std::string>(pat));
  }
  if (json::HasKey(j, "approval_patterns") && j.at("approval_patterns").is_array()) {
    for (const auto& pat : j.at("approval_patterns").as_array())
      if (pat.is_string())
        policy.approval_patterns.push_back(boost::json::value_to<std::string>(pat));
  }
  return policy;
}

Backend ParseBackend(const json::value& j) {
  Backend cfg;
  ReadBackendFields(cfg, j);
  // A stored backend keeps what the file resolved to, so it must not expand twice.
  cfg.host = ExpandEnvVars(cfg.host);
  cfg.model = ExpandEnvVars(cfg.model);
  if (cfg.api_key) cfg.api_key = ExpandEnvVars(*cfg.api_key);
  if (json::HasKey(j, "system_prompt"))
    cfg.system_prompt = ExpandEnvVars(boost::json::value_to<std::string>(j.at("system_prompt")));
  return cfg;
}

std::vector<McpServerConfig> ParseMcpServers(const json::value& j) {
  std::vector<McpServerConfig> servers;
  if (!j.is_array()) return servers;
  for (const auto& item : j.as_array()) {
    McpServerConfig srv;
    srv.name = json::ValueOrDefault<std::string>(item, "name", "");
    srv.command = json::ValueOrDefault<std::string>(item, "command", "");
    if (json::HasKey(item, "args") && item.at("args").is_array()) {
      for (const auto& a : item.at("args").as_array())
        if (a.is_string()) srv.args.push_back(boost::json::value_to<std::string>(a));
    }
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

AgentEntry ParseAgentEntry(const json::value& j) {
  AgentEntry entry;
  entry.name = json::ValueOrDefault<std::string>(j, "name", "");
  if (entry.name.empty()) throw pu::GenericError("Missing agent name");
  entry.description = json::ValueOrDefault<std::string>(j, "description", "");
  if (!json::HasKey(j, "backend") || !j.at("backend").is_object())
    throw pu::GenericError("Missing backend field");
  entry.backend = ParseBackend(j.at("backend"));
  if (entry.backend.host.empty()) entry.backend.host = DefaultHostFor(entry.backend.gateway);
  if (entry.backend.model.empty()) throw pu::GenericError("Missing model in backend");

  if (json::HasKey(j, "security") && j.at("security").is_object())
    entry.security = ParseSecurityPolicy(j.at("security"));
  if (json::HasKey(j, "mcp_servers") && j.at("mcp_servers").is_array())
    entry.mcp_servers = ParseMcpServers(j.at("mcp_servers"));
  entry.tool_call_budget = json::OptionalOrDefault<int>(j, "tool_call_budget");

  return entry;
}

}  // unnamed namespace

namespace {

// A proxy gateway's name belongs in the identifier; a vendor's own API leaves the model id.
std::string AgentNameFor(const llm::ModelEntry& model, const std::string& gateway) {
  if (gateway == "codebuddy") return gateway + "-" + model.id;
  return model.id;
}

std::string StringMember(const boost::json::object& object, const char* key) {
  const auto it = object.find(key);
  if (it == object.end() || !it->value().is_string()) return {};
  return boost::json::value_to<std::string>(it->value());
}

}  // unnamed namespace

std::string FormatAgentEntry(const llm::ModelEntry& model, const boost::json::value& source) {
  const boost::json::object* src = source.if_object();
  if (src == nullptr) throw GenericError("Cannot render an entry from something that is not an object");

  const boost::json::object* backend = nullptr;
  if (const auto it = src->find("backend"); it != src->end()) backend = it->value().if_object();
  if (backend == nullptr)
    throw GenericError("The agent this command runs as carries no backend to copy");

  const std::string gateway = StringMember(*backend, "gateway");

  boost::json::object entry;
  entry["name"] = AgentNameFor(model, gateway);

  std::string description = model.id + " via " + gateway;
  if (!model.caps.credits.empty()) description += ", " + model.caps.credits;
  entry["description"] = std::move(description);

  boost::json::object copied = *backend;
  copied["model"] = model.id;
  // A reasoning-only model rejects a request carrying no effort; a named level is kept.
  if (model.caps.only_reasoning && !model.caps.default_effort.empty()) {
    const std::string named = StringMember(copied, "thinking");
    if (named.empty() || named == "default") copied["thinking"] = model.caps.default_effort;
  }
  entry["backend"] = std::move(copied);

  for (const char* key : {"security", "mcp_servers", "tool_call_budget"}) {
    if (const auto it = src->find(key); it != src->end()) entry[key] = it->value();
  }

  return json::PrettyPrint(boost::json::value(std::move(entry)));
}

std::optional<boost::json::value> FindRawAgentEntry(const std::string& config_path,
                                                    const std::string& agent_name) {
  std::ifstream file(config_path);
  if (!file.is_open()) return std::nullopt;

  std::ostringstream buffer;
  buffer << file.rdbuf();

  json::value parsed;
  try {
    parsed = json::parse(buffer.str());
  } catch (const boost::system::system_error&) {
    return std::nullopt;
  }

  const boost::json::array* agents = nullptr;
  if (const boost::json::object* top = parsed.if_object()) {
    if (const auto it = top->find("agents"); it != top->end()) agents = it->value().if_array();
  }
  if (agents == nullptr) return std::nullopt;

  for (const auto& entry : *agents) {
    const boost::json::object* fields = entry.if_object();
    if (fields == nullptr) continue;
    const auto it = fields->find("name");
    if (it == fields->end() || !it->value().is_string()) continue;
    if (boost::json::value_to<std::string>(it->value()) == agent_name) return entry;
  }
  return std::nullopt;
}

std::string FindConfigPath() {
  const std::filesystem::path project = "./.pu/agents.json";
  if (std::filesystem::exists(project)) return project.string();

  const char* home = std::getenv("HOME");
  if (home) {
    const std::filesystem::path user = std::filesystem::path(home) / ".pu" / "agents.json";
    if (std::filesystem::exists(user)) return user.string();
  }

  return {};
}

std::optional<ServeOptions> FindServeOptions() {
  const std::string path = FindConfigPath();
  if (path.empty()) return std::nullopt;

  std::ifstream file(path);
  if (!file.is_open()) return std::nullopt;

  json::value j;
  try {
    std::ostringstream buffer;
    buffer << file.rdbuf();
    j = json::parse(buffer.str());
  } catch (const boost::system::system_error&) {
    return std::nullopt;
  }

  if (!json::HasKey(j, "serve") || !j.at("serve").is_object()) return std::nullopt;
  const json::value& serve = j.at("serve");

  ServeOptions options;
  if (json::HasKey(serve, "bind") && serve.at("bind").is_string()) {
    options.bind = ExpandEnvVars(boost::json::value_to<std::string>(serve.at("bind")));
    if (options.bind->empty()) options.bind = std::nullopt;
  }
  if (json::HasKey(serve, "port")) {
    if (serve.at("port").is_int64()) {
      const auto port = serve.at("port").as_int64();
      if (port >= 1 && port <= 65535) {
        options.port = static_cast<std::uint16_t>(port);
      } else {
        spdlog::warn("Ignoring serve.port {}: not a port number", port);
      }
    } else {
      spdlog::warn("Ignoring serve.port: not a whole number");
    }
  }

  if (!options.bind && !options.port) return std::nullopt;
  return options;
}

std::optional<std::uint16_t> ParsePort(const std::string& text) {
  if (text.empty() || text.size() > 5) return std::nullopt;
  std::uint32_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + static_cast<std::uint32_t>(c - '0');
  }
  if (value < 1 || value > 65535) return std::nullopt;
  return static_cast<std::uint16_t>(value);
}

ListenOptions ResolveListenOptions(const std::optional<std::string>& flag_bind,
                                   const std::optional<std::string>& flag_port,
                                   const std::optional<std::string>& env_bind,
                                   const std::optional<std::string>& env_port,
                                   const std::optional<ServeOptions>& from_file) {
  ListenOptions options;
  if (flag_bind || env_bind) {
    options.bind = flag_bind ? *flag_bind : *env_bind;
  } else if (from_file && from_file->bind) {
    options.bind = *from_file->bind;
  }

  const std::optional<std::string>* named = nullptr;
  const char* source = "";
  if (flag_port) {
    named = &flag_port;
    source = "--port";
  } else if (env_port) {
    named = &env_port;
    source = "PU_SERVE_PORT";
  }

  if (named == nullptr) {
    if (from_file && from_file->port) options.port = *from_file->port;
  } else if (const auto port = ParsePort(**named)) {
    options.port = *port;
  } else {
    std::cerr << "Warning: invalid " << source << " '" << **named << "', using " << options.port
              << "\n";
  }
  return options;
}

AgentsConfig LoadAgentsConfig(const std::string& config_path) {
  AgentsConfig result;
  std::ifstream file(config_path);
  if (!file.is_open()) throw pu::GenericError("Configuration file not found: " + config_path);

  json::value j;
  try {
    std::ostringstream buffer;
    buffer << file.rdbuf();
    j = json::parse(buffer.str());
  } catch (const boost::system::system_error&) {
    throw pu::GenericError("Failed to parse configuration JSON");
  }

  if (!json::HasKey(j, "default_agent") || !j.at("default_agent").is_string())
    throw pu::GenericError("Missing default_agent field");
  result.default_agent = boost::json::value_to<std::string>(j.at("default_agent"));

  if (!json::HasKey(j, "agents") || !j.at("agents").is_array())
    throw pu::GenericError("Missing agents array");
  for (const auto& item : j.at("agents").as_array()) result.agents.push_back(ParseAgentEntry(item));

  const auto default_agent =
      std::find_if(result.agents.begin(), result.agents.end(),
                   [&](const AgentEntry& entry) { return entry.name == result.default_agent; });
  if (default_agent == result.agents.end())
    throw pu::GenericError("default_agent does not match any configured agent");
  return result;
}

}  // namespace pu::config
