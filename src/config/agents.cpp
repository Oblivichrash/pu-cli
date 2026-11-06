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

paging::Budget ParsePaging(const json::value& j) {
  paging::Budget budget;
  if (json::HasKey(j, "page_lines") && j.at("page_lines").is_number())
    budget.page_lines = boost::json::value_to<std::size_t>(j.at("page_lines"));
  if (json::HasKey(j, "max_lines") && j.at("max_lines").is_number())
    budget.max_lines = boost::json::value_to<std::size_t>(j.at("max_lines"));
  if (json::HasKey(j, "capture_bytes") && j.at("capture_bytes").is_number())
    budget.capture_bytes = boost::json::value_to<std::size_t>(j.at("capture_bytes"));
  if (json::HasKey(j, "retain_bytes") && j.at("retain_bytes").is_number())
    budget.retain_bytes = boost::json::value_to<std::size_t>(j.at("retain_bytes"));
  if (json::HasKey(j, "retain_buffers") && j.at("retain_buffers").is_number())
    budget.retain_buffers = boost::json::value_to<std::size_t>(j.at("retain_buffers"));

  // A page is a page: a request is clamped to `max_lines`, so a `page_lines` above it
  // would be silently narrowed. Nothing retained keeps nothing, and a store that keeps
  // nothing cannot be paged at all.
  budget.page_lines = std::max<std::size_t>(1, budget.page_lines);
  budget.max_lines = std::max(budget.page_lines, budget.max_lines);
  budget.capture_bytes = std::max<std::size_t>(1024, budget.capture_bytes);
  budget.retain_bytes = std::max<std::size_t>(1024, budget.retain_bytes);
  budget.retain_buffers = std::max<std::size_t>(1, budget.retain_buffers);
  return budget;
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
  entry.description = json::ValueOrDefault<std::string>(j, "description", "");
  if (!json::HasKey(j, "backend") || !j.at("backend").is_object())
    throw pu::GenericError("Missing backend field");
  entry.backend = ParseBackend(j.at("backend"));
  if (entry.backend.host.empty()) entry.backend.host = DefaultHostFor(entry.backend.gateway);
  if (entry.backend.model.empty()) throw pu::GenericError("Missing model in backend");

  if (json::HasKey(j, "security") && j.at("security").is_object())
    entry.security = ParseSecurityPolicy(j.at("security"));
  if (json::HasKey(j, "paging") && j.at("paging").is_object())
    entry.paging = ParsePaging(j.at("paging"));
  if (json::HasKey(j, "mcp_servers") && j.at("mcp_servers").is_array())
    entry.mcp_servers = ParseMcpServers(j.at("mcp_servers"));
  entry.tool_call_budget = json::OptionalOrDefault<int>(j, "tool_call_budget");

  if (json::HasKey(j, "delegates")) {
    const json::value& list = j.at("delegates");
    if (!list.is_array()) throw pu::GenericError("delegates must be an array of agent names");
    for (const auto& item : list.as_array()) {
      if (!item.is_string()) throw pu::GenericError("delegates must be an array of agent names");
      entry.delegates.push_back(boost::json::value_to<std::string>(item));
    }
  }

  if (json::HasKey(j, "tools")) {
    const json::value& list = j.at("tools");
    if (!list.is_array()) throw pu::GenericError("tools must be an array of tool names");
    for (const auto& item : list.as_array()) {
      if (!item.is_string()) throw pu::GenericError("tools must be an array of tool names");
      const std::string name = boost::json::value_to<std::string>(item);
      const auto& known = BuiltinToolNames();
      if (std::find(known.begin(), known.end(), name) == known.end()) {
        std::string listed;
        for (const std::string& tool : known) listed += (listed.empty() ? "" : ", ") + tool;
        throw pu::GenericError("tools names '" + name + "', which is not a builtin tool (" +
                               listed + ")");
      }
      entry.tools.push_back(name);
    }
  }

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

// Only a path that names something the table's own directory holds is rewritten: an arguments
// list also carries flags and plain words, and turning `--verbose` into an absolute path would
// break every server that has one.
std::string ResolveAgainst(const std::filesystem::path& dir, const std::string& value) {
  if (value.empty()) return value;
  const std::filesystem::path path(value);
  if (path.is_absolute()) return value;
  std::error_code ec;
  if (!std::filesystem::exists(dir / path, ec)) return value;
  return (dir / path).lexically_normal().string();
}

json::value ReadJsonFile(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) throw pu::GenericError("Configuration file not found: " + path.string());
  std::ostringstream buffer;
  buffer << file.rdbuf();
  try {
    return json::parse(buffer.str());
  } catch (const boost::system::system_error&) {
    throw pu::GenericError("Failed to parse configuration JSON: " + path.string());
  }
}

// An agent whose table names a file reads it, and the file's text becomes the entry's system
// prompt. Naming both is refused rather than ordered: which one wins is a rule a reader would
// have to know, and there is no reason for a file to say the same thing twice.
void ApplyPersona(AgentEntry& entry, const json::value& table,
                  const std::filesystem::path& table_dir) {
  const bool names_file = json::HasKey(table, "persona");
  const bool inline_prompt = json::HasKey(table, "backend") &&
                             json::HasKey(table.at("backend"), "system_prompt");
  if (!names_file) return;
  if (!table.at("persona").is_string() || table.at("persona").get_string().empty()) {
    throw pu::GenericError("persona must be a path, in the table of " + entry.name);
  }
  if (inline_prompt) {
    throw pu::GenericError("the table of " + entry.name +
                           " names both persona and backend.system_prompt; keep one");
  }

  const std::filesystem::path declared(boost::json::value_to<std::string>(table.at("persona")));
  const std::filesystem::path path = declared.is_absolute() ? declared : table_dir / declared;

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    throw pu::GenericError("persona file not found for " + entry.name + ": " + path.string());
  }
  std::ostringstream text;
  text << file.rdbuf();
  std::string prompt = text.str();
  // The reader wrote a document, not a prompt: the trailing newline of the last line is the
  // file's own shape and not something the model should be handed as part of what it was told.
  while (!prompt.empty() && (prompt.back() == '\n' || prompt.back() == '\r')) prompt.pop_back();
  if (prompt.empty()) {
    throw pu::GenericError("persona file for " + entry.name + " is empty: " + path.string());
  }
  entry.backend.system_prompt = ExpandEnvVars(prompt);
}

std::filesystem::path AgentTableDir(const std::filesystem::path& config_path) {
  return config_path.parent_path() / "agents";
}

const std::vector<std::string>& BuiltinToolNames() {
  // The names `RegisterBuiltinTools` registers. A tool added there and not here is one a
  // `tools` list cannot name, which is the direction that fails loudly rather than silently.
  static const std::vector<std::string> names{"bash", "write_file", "ask_dir"};
  return names;
}

AgentsConfig LoadAgentsConfig(const std::string& config_path) {
  AgentsConfig result;
  const json::value j = ReadJsonFile(config_path);

  if (!json::HasKey(j, "default_agent") || !j.at("default_agent").is_string())
    throw pu::GenericError("Missing default_agent field");
  result.default_agent = boost::json::value_to<std::string>(j.at("default_agent"));

  if (!json::HasKey(j, "agents") || !j.at("agents").is_array())
    throw pu::GenericError("Missing agents array");
  for (const auto& item : j.at("agents").as_array()) {
    AgentEntry entry = ParseAgentEntry(item);
    if (entry.name.empty()) throw pu::GenericError("Missing agent name");
    result.agents.push_back(std::move(entry));
  }

  // One directory per agent, beside the table that names them. The directory name is the
  // agent's name, so `ls .pu/agents/` is the list, and a table that disagrees with the name it
  // was filed under is refused rather than silently renamed.
  const std::filesystem::path agents_dir = AgentTableDir(config_path);
  std::error_code ec;
  if (std::filesystem::is_directory(agents_dir, ec)) {
    std::vector<std::filesystem::path> tables;
    for (const auto& item : std::filesystem::directory_iterator(agents_dir, ec)) {
      if (!item.is_directory()) continue;
      const std::filesystem::path table = item.path() / "agents.json";
      if (std::filesystem::exists(table)) tables.push_back(table);
    }
    std::sort(tables.begin(), tables.end());

    for (const std::filesystem::path& table : tables) {
      const json::value dir_json = ReadJsonFile(table);
      AgentEntry entry = ParseAgentEntry(dir_json);
      const std::string from_dir = table.parent_path().filename().string();
      if (json::HasKey(dir_json, "name") && entry.name != from_dir) {
        throw pu::GenericError("the table in agents/" + from_dir + " names itself '" +
                               entry.name + "'; a directory is named after its agent");
      }
      entry.name = from_dir;
      const auto clash = std::find_if(result.agents.begin(), result.agents.end(),
                                      [&](const AgentEntry& known) { return known.name == entry.name; });
      if (clash != result.agents.end()) {
        throw pu::GenericError("two agents are named '" + entry.name + "': one inline in " +
                               config_path + ", one in " + table.string());
      }
      entry.source_dir = table.parent_path();
      ApplyPersona(entry, dir_json, table.parent_path());
      for (config::McpServerConfig& server : entry.mcp_servers) {
        server.command = ResolveAgainst(table.parent_path(), server.command);
        for (std::string& arg : server.args) arg = ResolveAgainst(table.parent_path(), arg);
      }
      if (entry.backend.host.empty()) entry.backend.host = DefaultHostFor(entry.backend.gateway);
      if (entry.backend.model.empty()) throw pu::GenericError("Missing model in backend of " + entry.name);
      result.agents.push_back(std::move(entry));
    }
  }

  const auto default_agent =
      std::find_if(result.agents.begin(), result.agents.end(),
                   [&](const AgentEntry& entry) { return entry.name == result.default_agent; });
  if (default_agent == result.agents.end()) {
    std::string known;
    for (const AgentEntry& entry : result.agents) known += (known.empty() ? "" : ", ") + entry.name;
    throw pu::GenericError("default_agent does not match any configured agent (have: " + known +
                           ")");
  }
  return result;
}

}  // namespace pu::config
