// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/agents.hpp"

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"
#include "pu/model/gateway.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <regex>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

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

// The strings one array member holds, in order, skipping anything that is not a string. An
// absent member and an empty one answer the same way: no entries.
std::vector<std::string> StringList(const json::value& j, boost::json::string_view key) {
  std::vector<std::string> entries;
  for (const auto& item : json::ValueOrDefault<boost::json::array>(j, key, {})) {
    if (item.is_string()) entries.push_back(boost::json::value_to<std::string>(item));
  }
  return entries;
}

SecurityPolicy ParseSecurityPolicy(const json::value& j) {
  SecurityPolicy policy;
  policy.sandbox_root = json::ValueOrDefault<std::string>(j, "sandbox_root", policy.sandbox_root);
  policy.max_command_length =
      json::ValueOrDefault<std::size_t>(j, "max_command_length", policy.max_command_length);
  policy.forbidden_patterns = StringList(j, "forbidden_patterns");
  policy.approval_patterns = StringList(j, "approval_patterns");
  return policy;
}

paging::Budget ParsePaging(const json::value& j) {
  paging::Budget budget;
  budget.page_lines = json::ValueOrDefault<std::size_t>(j, "page_lines", budget.page_lines);
  budget.max_lines = json::ValueOrDefault<std::size_t>(j, "max_lines", budget.max_lines);
  budget.page_bytes = json::ValueOrDefault<std::size_t>(j, "page_bytes", budget.page_bytes);
  budget.capture_bytes = json::ValueOrDefault<std::size_t>(j, "capture_bytes", budget.capture_bytes);
  budget.retain_bytes = json::ValueOrDefault<std::size_t>(j, "retain_bytes", budget.retain_bytes);
  budget.retain_buffers =
      json::ValueOrDefault<std::size_t>(j, "retain_buffers", budget.retain_buffers);

  // A page is a page: a request is clamped to `max_lines`, so a `page_lines` above it
  // would be silently narrowed. Nothing retained keeps nothing, and a store that keeps
  // nothing cannot be paged at all.
  budget.page_lines = std::max<std::size_t>(1, budget.page_lines);
  budget.max_lines = std::max(budget.page_lines, budget.max_lines);
  // A page bound of zero would make every page empty, which is not the same as unbounded.
  budget.page_bytes = std::max<std::size_t>(1024, budget.page_bytes);
  budget.capture_bytes = std::max<std::size_t>(1024, budget.capture_bytes);
  budget.retain_bytes = std::max<std::size_t>(1024, budget.retain_bytes);
  budget.retain_buffers = std::max<std::size_t>(1, budget.retain_buffers);
  return budget;
}

model::Backend ParseBackend(const json::value& j) {
  model::Backend cfg;
  model::ReadBackendFields(cfg, j);
  // A stored backend keeps what the file resolved to, so it must not expand twice.
  cfg.host = ExpandEnvVars(cfg.host);
  cfg.model = ExpandEnvVars(cfg.model);
  if (cfg.api_key) {
    cfg.api_key = ExpandEnvVars(*cfg.api_key);
  } else {
    // The gateway's own variable, when the file names no key at all. A backend that says
    // which product it talks to and not which key it uses is the ordinary case: the key is a
    // secret in the environment, and writing the variable's name in every entry is asking the
    // file to restate what the gateway already knows. Only an absent `api_key` reaches here —
    // an explicit one, even one that expanded to empty, is the author's answer and is left as
    // they wrote it, so a typo in a variable name stays visible instead of being papered over
    // by a second lookup.
    const std::string_view env = model::ApiKeyEnvFor(cfg.gateway);
    if (!env.empty()) {
      const std::string name(env);
      if (const char* value = std::getenv(name.c_str()); value != nullptr && *value != '\0') {
        cfg.api_key = std::string(value);
      }
    }
  }
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
    srv.timeout_ms = json::ValueOrDefault<int>(item, "timeout_ms", srv.timeout_ms);
    if (srv.timeout_ms <= 0) {
      spdlog::warn("MCP server '{}' asked for timeout_ms={}; using the default", srv.name,
                   srv.timeout_ms);
      srv.timeout_ms = config::McpServerConfig{}.timeout_ms;
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
    throw pu::GenericError("Missing backend: an agent file describes one agent's backend");
  entry.backend = ParseBackend(j.at("backend"));
  if (entry.backend.host.empty()) entry.backend.host = model::DefaultHostFor(entry.backend.gateway);
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
// Which product that is, is the table's fact rather than this file's: the row says so, and a
// gateway added tomorrow gets the right answer without a line here.
std::string AgentNameFor(const model::ModelEntry& model, const std::string& gateway) {
  const model::Gateway* row = model::FindGateway(gateway);
  if (row != nullptr && row->prefixes_agent_name) return gateway + "-" + model.id;
  return model.id;
}

std::string StringMember(const boost::json::object& object, const char* key) {
  const auto it = object.find(key);
  if (it == object.end() || !it->value().is_string()) return {};
  return boost::json::value_to<std::string>(it->value());
}

}  // unnamed namespace

std::string FormatAgentEntry(const model::ModelEntry& model, const boost::json::value& source) {
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

  const boost::json::object* fields = parsed.if_object();
  if (fields == nullptr) return std::nullopt;
  // One file describes one agent, so a name that does not match its own is not a miss to search
  // past: it is the wrong file.
  const std::string own = StringMember(*fields, "name");
  if (own != agent_name) return std::nullopt;
  return parsed;
}

std::string FindConfigPath() {
  // Read against the run's own root, not the shell's: `pu -C ..` asks about the directory it
  // named, and a file sitting there is the one it means. Falling back to the working
  // directory would find nothing in that case and report a configured workspace as a first
  // run, which is what sends `-C` into the setup wizard.
  const std::filesystem::path project = pu::WorkspaceRoot() / ".pu" / "agents.json";
  if (std::filesystem::exists(project)) return project.string();

  const char* home = std::getenv("HOME");
  if (home) {
    const std::filesystem::path user = std::filesystem::path(home) / ".pu" / "agents.json";
    if (std::filesystem::exists(user)) return user.string();
  }

  return {};
}

// The configuration is read and written by more than one process: `pu serve` holds it open for
// the length of a run, and a second process -- the reader running `pu models --set`, a page save
// against a serve that is already running -- writes the same file. The lock is taken on a
// sibling `.lock` file rather than on the configuration itself, because the write replaces the
// configuration by rename, and a lock follows the inode: the replacement would drop it and leave
// the window it was meant to close.
//
// The lock is held only across the read-modify-write. A reader never takes it, since a rename is
// atomic and a reader sees either the old file or the new one, never half of either.
class ConfigLock {
 public:
  explicit ConfigLock(const std::string& path) {
#ifdef _WIN32
    const std::string lock_path = path + ".lock";
    handle_ = CreateFileA(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) held_ = LockHeld(*this);
#else
    const std::string lock_path = path + ".lock";
    fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    held_ = fd_ >= 0 && ::flock(fd_, LOCK_EX) == 0;
#endif
  }

  ~ConfigLock() { Release(); }

  ConfigLock(const ConfigLock&) = delete;
  ConfigLock& operator=(const ConfigLock&) = delete;

  void Release() {
    if (!held_) return;
    held_ = false;
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
      fd_ = -1;
    }
#endif
  }

 private:
#ifdef _WIN32
  // `LockFileEx` waits, which is what a second writer wants: the two saves queue rather than one
  // losing its change to the other's read-modify-write.
  static bool LockHeld(ConfigLock& self) {
    if (self.handle_ == INVALID_HANDLE_VALUE) return false;
    std::memset(&self.overlapped_, 0, sizeof(self.overlapped_));
    return ::LockFileEx(self.handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD,
                        &self.overlapped_) != 0;
  }
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  OVERLAPPED overlapped_{};
#else
  int fd_ = -1;
#endif
  bool held_ = false;
};

// Written beside the target and renamed over it: `rename` is atomic on POSIX, and on Windows
// `std::filesystem::rename` refuses a target that already exists, which is the failure that made
// the write look like it had happened when it had not. `MoveFileEx` with REPLACE_EXISTING is the
// same operation there. The staged file is named for the process so two writers that did not wait
// for the lock cannot publish one over the other's half-written text.
void ReplaceFileText(const std::string& path, const std::string& text) {
  const std::string staged = path + "." + std::to_string(platform::CurrentProcessId()) + ".tmp";
  {
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) throw pu::GenericError("cannot write " + staged);
    out << text;
    out.flush();
    if (!out) throw pu::GenericError("cannot write " + staged);
  }
#ifdef _WIN32
  // The wide call, because the ANSI one reads the paths in the machine's code page and loses any
  // character outside it -- a user name, a temporary directory that is not ASCII. Moving over an
  // existing file needs the target to be closed by everyone, which is why the reader in
  // `UpdateAgentBackend` closes before it gets here; when something else still holds it, the move
  // fails and the file is replaced by deleting it first rather than left unwritten.
  if (!::MoveFileExW(std::filesystem::path(staged).wstring().c_str(),
                     std::filesystem::path(path).wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) {
    std::error_code ignored;
    if (!::DeleteFileW(std::filesystem::path(path).wstring().c_str()) ||
        !::MoveFileW(std::filesystem::path(staged).wstring().c_str(),
                     std::filesystem::path(path).wstring().c_str())) {
      std::filesystem::remove(staged, ignored);
      throw pu::GenericError("cannot replace " + path);
    }
  }
#else
  std::error_code ec;
  std::filesystem::rename(staged, path, ec);
  if (ec) {
    std::filesystem::remove(staged, ec);
    throw pu::GenericError("cannot replace " + path);
  }
#endif
}

void UpdateAgentBackend(const std::string& config_path, const BackendUpdate& update) {
  ConfigLock lock(config_path);
  std::string original;
  {
    std::ifstream file(config_path);
    if (!file.is_open()) throw pu::GenericError("cannot open " + config_path);
    original.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  std::optional<json::value> parsed = json::TryParse(original);
  if (!parsed) throw pu::GenericError(config_path + " is not JSON");
  json::value& root = *parsed;
  if (!root.is_object()) throw pu::GenericError(config_path + " is not a JSON object");
  if (!json::HasKey(root, "backend") || !root.at("backend").is_object())
    throw pu::GenericError(config_path + " has no backend to change");
  boost::json::object& backend = root.at("backend").as_object();
  backend["gateway"] = update.gateway;
  backend["model"] = update.model;
  // The address is written as the reader spelled it. `${VAR}` is left standing so the file keeps
  // naming the environment rather than this run's expansion of it, and an empty value deletes the
  // key: a host key holding "" would pin the empty address, while no key at all is what lets the
  // gateway supply its own default on the next load.
  if (update.host.empty()) {
    backend.erase("host");
  } else {
    backend["host"] = update.host;
  }
  // The sampling settings follow the address' rule: stated, they are written; not stated, the key
  // is gone. A key holding a number nobody chose is worse than no key, because the file then
  // claims a pin the reader never asked for. `temperature` is the exception: it is not optional
  // in the type, so a backend always answers at some value and the file always says which.
  backend["temperature"] = update.temperature;
  // `thinking` is the other exception, and for the same reason: `kServerDefault` is a level the
  // file spells `default` rather than a missing key, so an absent level is written as the word
  // the loader reads back as "no level". Writing holes here would make "the backend decides" and
  // "nobody said" two different states the loader cannot tell apart.
  backend["thinking"] = update.thinking;
  if (update.max_tokens) {
    backend["max_tokens"] = *update.max_tokens;
  } else {
    backend.erase("max_tokens");
  }
  if (update.top_p) {
    backend["top_p"] = *update.top_p;
  } else {
    backend.erase("top_p");
  }
  if (update.seed) {
    backend["seed"] = *update.seed;
  } else {
    backend.erase("seed");
  }
  if (update.context_window) {
    backend["context_window"] = *update.context_window;
  } else {
    backend.erase("context_window");
  }

  const std::string text = boost::json::serialize(root);
  // Read it back before it replaces anything: a file this program would refuse to load is never
  // left on disk, so a mistake here costs the command that made it and not the next start.
  std::optional<json::value> written = json::TryParse(text);
  if (!written) throw pu::GenericError(config_path + " would not read back as JSON");
  ParseAgentEntry(*written);

  ReplaceFileText(config_path, text);
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
  std::optional<json::value> parsed = json::TryParse(buffer.str());
  if (!parsed) throw pu::GenericError("Failed to parse configuration JSON: " + path.string());
  return std::move(*parsed);
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

const std::vector<std::string>& BuiltinToolNames() {
  // The names `RegisterBuiltinTools` registers. A tool added there and not here is one a
  // `tools` list cannot name, which is the direction that fails loudly rather than silently.
  static const std::vector<std::string> names{"bash", "write_file", "ask_dir"};
  return names;
}

AgentEntry LoadAgentConfig(const std::string& config_path) {
  const json::value j = ReadJsonFile(config_path);
  AgentEntry entry = ParseAgentEntry(j);
  if (entry.name.empty()) {
    throw pu::GenericError(config_path +
                           " describes one agent, so it must name itself: add a \"name\" field");
  }
  const std::filesystem::path dir = std::filesystem::path(config_path).parent_path();
  entry.source_dir = dir;
  ApplyPersona(entry, j, dir);
  for (config::McpServerConfig& server : entry.mcp_servers) {
    server.command = ResolveAgainst(dir, server.command);
    for (std::string& arg : server.args) arg = ResolveAgainst(dir, arg);
  }
  return entry;
}

// One key per field a parser above reads. A field added to a parser and not to its list here is
// the one mistake this arrangement can still make, so each list is written directly under its
// parser: the edit that adds a field is an edit to a parser, and the list is what the reader of
// that function checks against.
const std::set<std::string>& ReadAgentKeys() {
  static const std::set<std::string> keys{"name",      "description", "backend",
                                          "security",  "paging",      "mcp_servers",
                                          "tool_call_budget", "delegates", "tools",
                                          "persona"};
  return keys;
}

const std::set<std::string>& ReadBackendKeys() {
  // `ReadBackendFields` in `model::Backend` is what reads these; the system prompt is added by
  // `ParseBackend`, which is why it is here rather than there.
  static const std::set<std::string> keys{"gateway",    "host",        "model",
                                          "api_key",    "temperature", "system_prompt",
                                          "max_tokens", "thinking",    "context_window"};
  return keys;
}

const std::set<std::string>& ReadSecurityKeys() {
  static const std::set<std::string> keys{"sandbox_root", "max_command_length",
                                          "forbidden_patterns", "approval_patterns"};
  return keys;
}

const std::set<std::string>& ReadPagingKeys() {
  static const std::set<std::string> keys{"page_lines", "max_lines", "page_bytes",
                                          "capture_bytes", "retain_bytes", "retain_buffers"};
  return keys;
}

const std::set<std::string>& ReadMcpServerKeys() {
  static const std::set<std::string> keys{"name", "command", "args", "url", "headers",
                                          "timeout_ms"};
  return keys;
}

}  // namespace pu::config
