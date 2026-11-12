// SPDX-License-Identifier: GPL-3.0-only

//
// What a configuration file resolves to, printed without asking anything of a provider.
//
// Three things turn a written config into the values that actually run: `${ENV}` expansion,
// a gateway's default host, and the workspace a sandbox root is relative to. A file can be
// wrong at any of them and look right at every one — a config that names the wrong gateway
// fails as a refused connection, and an unset variable fails as an unauthorized key. This
// prints the resolved values so the mistake is read rather than deduced from a failed turn.
//

#include "pu/app/cli.hpp"

#include <boost/json.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pu/config/agents.hpp"
#include "pu/config/backend.hpp"
#include "pu/config/gateway.hpp"
#include "pu/core/json.hpp"
#include "pu/core/text.hpp"

namespace pu::cli {

namespace {

namespace fs = std::filesystem;

// Everything ParseAgentEntry reads. A key outside this set is read by nothing, which is the
// one mistake a file cannot show by running: the field is simply not there any more.
const std::set<std::string>& ReadAgentKeys() {
  static const std::set<std::string> keys{"name",     "description",    "backend", "security",
                                          "paging",   "mcp_servers",    "tool_call_budget",
                                          "delegates", "tools",         "persona"};
  return keys;
}

const std::set<std::string>& ReadBackendKeys() {
  static const std::set<std::string> keys{"gateway",      "host",           "model",
                                          "api_key",      "temperature",    "system_prompt",
                                          "max_tokens",   "thinking",       "context_window"};
  return keys;
}

const std::set<std::string>& ReadSecurityKeys() {
  static const std::set<std::string> keys{"sandbox_root", "max_command_length",
                                          "forbidden_patterns", "approval_patterns"};
  return keys;
}

const std::set<std::string>& ReadPagingKeys() {
  static const std::set<std::string> keys{"page_lines", "max_lines", "capture_bytes",
                                          "retain_bytes", "retain_buffers"};
  return keys;
}

// A key the parser never looks at. Reported rather than refused: this build may be older
// than the file, and a warning is right when the user cannot tell which.
void WarnIgnoredKeys(const boost::json::object& object, const std::set<std::string>& read,
                     const std::string& where, std::vector<std::string>& warnings) {
  for (const auto& [key, value] : object) {
    (void)value;
    if (read.count(std::string(key)) == 0) {
      warnings.push_back("field '" + std::string(key) + "' in " + where +
                         " is not read by this build");
    }
  }
}

std::string ApiKeyState(const config::Backend& backend, const boost::json::value& raw_backend,
                        std::vector<std::string>& warnings) {
  if (!backend.api_key) return "(none)";
  if (backend.api_key->empty()) {
    // What an unset ${VAR} expands to: an empty key, which a provider answers with a 401
    // rather than with "that variable is not set".
    const std::string declared = json::ValueOrDefault<std::string>(raw_backend, "api_key", "");
    warnings.push_back("api_key in " + std::string("this backend") + " resolves to empty" +
                       (declared.empty() ? std::string()
                                         : " (declared as " + declared +
                                               "; is the variable set?)"));
    return "(empty)";
  }
  return "(set, " + std::to_string(backend.api_key->size()) + " characters)";
}

std::string SandboxState(const config::SecurityPolicy& policy, const fs::path& workspace,
                         std::vector<std::string>& warnings) {
  if (policy.sandbox_root.empty()) {
    warnings.push_back("no sandbox_root names, so the workspace is the sandbox");
    return workspace.string();
  }
  const fs::path resolved = fs::path(policy.sandbox_root).is_absolute()
                                ? fs::path(policy.sandbox_root)
                                : workspace / policy.sandbox_root;
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(resolved, ec);
  if (ec) {
    warnings.push_back("sandbox_root '" + policy.sandbox_root + "' does not resolve: " +
                       ec.message());
    return resolved.string() + " (unresolved)";
  }
  return canonical.string();
}

std::string FormatAgent(const config::AgentEntry& entry, const boost::json::value& raw,
                        const fs::path& workspace, const std::string& default_agent,
                        std::vector<std::string>& warnings) {
  const boost::json::object* raw_agent = raw.if_object();
  std::ostringstream out;
  out << entry.name;
  if (entry.name == default_agent) out << "   (default_agent)";
  out << "\n";

  if (!entry.description.empty()) out << "  description  " << entry.description << "\n";
  out << "  gateway      " << config::GatewayName(entry.backend.gateway) << "\n";
  out << "  host         " << entry.backend.host;
  // A host the file did not name came from the gateway's own default, which is worth
  // saying: it is the value a wrong gateway name silently produces.
  if (raw_agent != nullptr && raw_agent->contains("backend")) {
    const boost::json::value& raw_backend = raw_agent->at("backend");
    const std::string declared = json::ValueOrDefault<std::string>(raw_backend, "host", "");
    if (declared.empty()) out << "   (default for this gateway)";
    else if (declared != entry.backend.host) out << "   (from " << declared << ")";
  }
  out << "\n";
  out << "  model        " << entry.backend.model << "\n";
  if (raw_agent != nullptr && raw_agent->contains("backend")) {
    out << "  api_key      " << ApiKeyState(entry.backend, raw_agent->at("backend"), warnings)
        << "\n";
  }
  out << "  thinking     " << ThinkingLevelName(entry.backend.thinking) << "\n";
  if (entry.backend.max_tokens) {
    out << "  max_tokens   " << *entry.backend.max_tokens << "\n";
  }
  if (entry.backend.context_window) {
    out << "  context      " << *entry.backend.context_window << "\n";
  }
  out << "  sandbox      " << SandboxState(entry.security, workspace, warnings) << "\n";
  if (!entry.security.forbidden_patterns.empty()) {
    out << "  forbids      " << text::JoinQuoted(entry.security.forbidden_patterns) << "\n";
  }
  if (!entry.security.approval_patterns.empty()) {
    out << "  approval     " << text::JoinQuoted(entry.security.approval_patterns)
        << "   (no channel to ask on from the CLI: such a command is refused)\n";
  }
  if (!entry.mcp_servers.empty()) {
    out << "  mcp          "
        << text::Join(entry.mcp_servers, ", ", [](const config::McpServerConfig& server) {
             return server.name + " (" + (server.url.empty() ? server.command : server.url) + ")";
           })
        << "\n";
  }
  if (entry.tool_call_budget) {
    out << "  tool budget  " << *entry.tool_call_budget << "\n";
  }
  // What this one is allowed to change. The default is every builtin, which is the answer a
  // reader is most likely to be wrong about, so it is said rather than left blank.
  out << "  tools        ";
  if (entry.tools.empty()) {
    out << "all builtin (bash, write_file, ask_dir)\n";
  } else {
    out << text::Join(entry.tools) << "\n";
  }
  if (!entry.delegates.empty()) {
    out << "  delegates to " << text::Join(entry.delegates) << "\n";
    // A delegation is reached through `ask_dir`, and one whose `tools` list leaves that out
    // names a relationship nothing can use. Not an error — the table is legal — but the one
    // thing a reader would not see from either field alone.
    const bool has_ask_dir =
        entry.tools.empty() ||
        std::find(entry.tools.begin(), entry.tools.end(), "ask_dir") != entry.tools.end();
    if (!has_ask_dir) {
      warnings.push_back(entry.name +
                         " delegates to another agent but its tools leave out ask_dir, so the "
                         "delegation cannot be used");
    }
  }
  if (!entry.source_dir.empty()) out << "  table        " << entry.source_dir.string() << "\n";

  if (raw_agent != nullptr) WarnIgnoredKeys(*raw_agent, ReadAgentKeys(), entry.name, warnings);
  if (raw_agent != nullptr && raw_agent->contains("backend") &&
      raw_agent->at("backend").is_object()) {
    WarnIgnoredKeys(raw_agent->at("backend").as_object(), ReadBackendKeys(),
                    entry.name + ".backend", warnings);
  }
  if (raw_agent != nullptr && raw_agent->contains("security") &&
      raw_agent->at("security").is_object()) {
    WarnIgnoredKeys(raw_agent->at("security").as_object(), ReadSecurityKeys(),
                    entry.name + ".security", warnings);
  }
  if (raw_agent != nullptr && raw_agent->contains("paging") &&
      raw_agent->at("paging").is_object()) {
    WarnIgnoredKeys(raw_agent->at("paging").as_object(), ReadPagingKeys(),
                    entry.name + ".paging", warnings);
  }
  return out.str();
}

}  // namespace

int RunCheck(const std::string& config_path, const std::filesystem::path& workspace_in) {
  const fs::path workspace = workspace_in.empty() ? fs::current_path() : workspace_in;

  std::string path = config_path;
  if (path.empty()) {
    // The same order `Initialize` reads a run by, resolved against this directory rather than
    // the process's: a table beside the directory `-C` named is the one the run would load.
    const fs::path local = workspace / ".pu" / "agents.json";
    std::error_code ec;
    if (fs::exists(local, ec)) {
      path = local.string();
    } else if (const char* home = std::getenv("HOME"); home != nullptr) {
      const fs::path user = fs::path(home) / ".pu" / "agents.json";
      if (fs::exists(user, ec)) path = user.string();
    }
  }
  if (path.empty()) {
    std::cerr << "No agents.json found: looked in " << (workspace / ".pu").string()
              << "/ and ~/.pu/.\n";
    return static_cast<int>(ExitCode::kUsage);
  }

  std::cout << "config  " << path << "\n\n";

  config::AgentsConfig cfg;
  try {
    cfg = config::LoadAgentsConfig(path);
  } catch (const std::exception& e) {
    // A file this build cannot read is the invocation being wrong: fixing it is the remedy.
    std::cerr << "cannot load " << path << ": " << e.what() << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }

  std::cout << "workspace  " << workspace.string() << "\n";
  std::cout << "default    " << cfg.default_agent << "\n\n";

  const boost::json::value root = [&] {
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    try {
      return boost::json::parse(text.str());
    } catch (const std::exception&) {
      return boost::json::value(boost::json::object{});
    }
  }();
  const boost::json::array* raw_agents = nullptr;
  if (root.is_object() && root.as_object().contains("agents") &&
      root.at("agents").is_array()) {
    raw_agents = &root.at("agents").as_array();
  }

  std::vector<std::string> warnings;
  for (std::size_t i = 0; i < cfg.agents.size(); ++i) {
    const boost::json::value raw =
        (raw_agents != nullptr && i < raw_agents->size()) ? (*raw_agents)[i]
                                                         : boost::json::value(boost::json::object{});
    std::cout << FormatAgent(cfg.agents[i], raw, workspace, cfg.default_agent, warnings) << "\n";
  }

  // The session outranks default_agent by design, so a session naming an agent this file no
  // longer has is a fact to report rather than a file to fix: the names that do exist are
  // printed beside it.
  // Beside the config that was read, which is where the runtime looks: a session and the
  // file that configures it are two halves of one workspace, and `--config` names one of
  // them only for the reader's convenience.
  const fs::path session_path = fs::path(path).parent_path() / "session.json";
  if (fs::exists(session_path)) {
    std::ifstream file(session_path);
    std::ostringstream text;
    text << file.rdbuf();
    boost::json::value saved;
    try {
      saved = boost::json::parse(text.str());
    } catch (const std::exception&) {
      saved = boost::json::value(boost::json::object{});
    }
    const std::string named =
        saved.is_object() && saved.as_object().contains("session_spec") &&
                saved.at("session_spec").is_object()
            ? json::ValueOrDefault<std::string>(saved.at("session_spec"), "agent_name", "")
            : std::string();
    if (!named.empty()) {
      const bool known = std::any_of(cfg.agents.begin(), cfg.agents.end(),
                                     [&](const config::AgentEntry& entry) {
                                       return entry.name == named;
                                     });
      std::cout << "session  " << session_path.string() << "\n";
      if (known) {
        std::cout << "  names  " << named << "\n\n";
      } else {
        std::cout << "  names  " << named << ", which this file does not configure\n";
        std::cout << "  configured: ";
        for (std::size_t i = 0; i < cfg.agents.size(); ++i) {
          if (i > 0) std::cout << ", ";
          std::cout << cfg.agents[i].name;
        }
        std::cout << "\n\n";
      }
    }
  }

  // Delegation is a relation between entries, so it is checked once against all of them: a
  // name nothing configures, an agent told it may ask itself, and a cycle are all facts the
  // single entry cannot show. Each is an error rather than a warning, because a table that
  // says it cannot be used is one a turn will fail on later with less to go on.
  std::set<std::string> configured;
  for (const config::AgentEntry& entry : cfg.agents) configured.insert(entry.name);

  std::vector<std::string> errors;
  for (const config::AgentEntry& entry : cfg.agents) {
    for (const std::string& target : entry.delegates) {
      if (configured.count(target) == 0) {
        errors.push_back(entry.name + " delegates to '" + target +
                         "', which nothing configures");
      } else if (target == entry.name) {
        errors.push_back(entry.name + " delegates to itself");
      }
    }
  }
  // A cycle would run out of depth rather than run forever, but it is still a table nobody
  // meant to write, and the chain is what makes it obvious.
  for (const config::AgentEntry& entry : cfg.agents) {
    std::vector<std::string> chain{entry.name};
    std::string here = entry.name;
    for (std::size_t step = 0; step < cfg.agents.size(); ++step) {
      const auto it = std::find_if(cfg.agents.begin(), cfg.agents.end(),
                                   [&](const config::AgentEntry& known) { return known.name == here; });
      if (it == cfg.agents.end() || it->delegates.empty()) break;
      here = it->delegates.front();
      if (here == entry.name) {
        std::string drawn;
        for (const std::string& name : chain) drawn += name + " -> ";
        errors.push_back("delegation goes round in a circle: " + drawn + entry.name);
        break;
      }
      if (std::find(chain.begin(), chain.end(), here) != chain.end()) break;
      chain.push_back(here);
    }
  }
  // One report per cycle, not one per member of it.
  std::sort(errors.begin(), errors.end());
  errors.erase(std::unique(errors.begin(), errors.end()), errors.end());

  if (!errors.empty()) {
    std::cout << "delegation\n";
    for (const std::string& error : errors) std::cout << "  " << error << "\n";
    std::cout << "\n";
  }

  if (!warnings.empty()) {
    std::cout << "warnings\n";
    for (const std::string& warning : warnings) std::cout << "  " << warning << "\n";
  }

  // A warning is not a failure: a file this build reads and an entry it can run is a pass,
  // and what it noticed is for a person to weigh. A delegation that names nothing runnable is
  // a failure, because the turn that reaches it fails with less to go on than this does.
  if (!errors.empty()) return static_cast<int>(ExitCode::kUsage);
  return static_cast<int>(ExitCode::kOk);
}

}  // namespace pu::cli
