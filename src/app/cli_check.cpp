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
#include "pu/model/backend.hpp"
#include "pu/model/gateway.hpp"
#include "pu/core/json.hpp"
#include "pu/core/text.hpp"
#include "pu/core/workspace_path.hpp"

namespace pu::cli {

namespace {

namespace fs = std::filesystem;

// Everything ParseAgentEntry reads. A key outside this set is read by nothing, which is the
// one mistake a file cannot show by running: the field is simply not there any more.
//
// The lists themselves live in `config/agents.cpp`, beside the parsers they describe, so a
// parser that learns a field and a report that does not is one edit rather than two files.

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

std::string ApiKeyState(const model::Backend& backend, const boost::json::value& raw_backend,
                        std::vector<std::string>& warnings) {
  if (!backend.api_key) return "(none)";
  if (backend.api_key->empty()) {
    // What an unset ${VAR} expands to: an empty key, which a provider answers with a 401
    // rather than with "that variable is not set".
    const std::string declared = json::ValueOrDefault<std::string>(raw_backend, "api_key", "");
    const std::string detail =
        declared.empty() ? std::string() : " (declared as " + declared + "; is the variable set?)";
    warnings.push_back("api_key in this backend resolves to empty" + detail);
    return "(empty)";
  }
  return "(set, " + std::to_string(backend.api_key->size()) + " characters)";
}

// Where the toolbox will actually be allowed to write, resolved by the same rule the runtime
// applies when it hands the policy to a tool -- not by a second copy of it. `weakly_canonical`
// is over the resolved path rather than the written one, so a `..` that climbs out of the
// workspace shows as the place it lands.
std::string SandboxState(const config::SecurityPolicy& policy, const fs::path& workspace,
                         std::vector<std::string>& warnings) {
  const fs::path resolved = ResolveWorkspacePath(workspace, policy.sandbox_root);
  std::error_code ec;
  const fs::path canonical = fs::weakly_canonical(resolved, ec);
  if (ec) {
    warnings.push_back("sandbox_root '" + policy.sandbox_root + "' does not resolve: " +
                       ec.message());
    return resolved.string() + " (unresolved)";
  }
  return canonical.string();
}

// The header carries the agent's name and nothing else.
std::string FormatAgent(const config::AgentEntry& entry, const boost::json::value* raw,
                        const fs::path& workspace, std::vector<std::string>& warnings) {
  const boost::json::object* raw_agent = raw == nullptr ? nullptr : raw->if_object();
  std::ostringstream out;
  out << entry.name << "\n";

  if (!entry.description.empty()) out << "  description  " << entry.description << "\n";
  out << "  gateway      " << model::GatewayName(entry.backend.gateway) << "\n";
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
  } else {
    // The resolved key is known from the environment whether or not the file could be read,
    // and a key that expanded to nothing is the failure this report exists to catch.
    out << "  api_key      "
        << (entry.backend.api_key ? "(set, " + std::to_string(entry.backend.api_key->size()) +
                                        " characters)"
                                  : "(none)")
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
    // A delegation is reached through `ask_dir`. `tools` leaving that out names a relationship
    // nothing can use; the file is still legal, so it is said and not refused.
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

  if (raw_agent != nullptr) WarnIgnoredKeys(*raw_agent, config::ReadAgentKeys(), entry.name,
                                            warnings);
  if (raw_agent != nullptr && raw_agent->contains("backend") &&
      raw_agent->at("backend").is_object()) {
    WarnIgnoredKeys(raw_agent->at("backend").as_object(), config::ReadBackendKeys(),
                    entry.name + ".backend", warnings);
  }
  if (raw_agent != nullptr && raw_agent->contains("security") &&
      raw_agent->at("security").is_object()) {
    WarnIgnoredKeys(raw_agent->at("security").as_object(), config::ReadSecurityKeys(),
                    entry.name + ".security", warnings);
  }
  if (raw_agent != nullptr && raw_agent->contains("paging") &&
      raw_agent->at("paging").is_object()) {
    WarnIgnoredKeys(raw_agent->at("paging").as_object(), config::ReadPagingKeys(),
                    entry.name + ".paging", warnings);
  }
  // Each server is its own object, so each is read for its own keys: a field named on one
  // server and not another is a fact about that one entry, and the index is how the report
  // says which.
  if (raw_agent != nullptr && raw_agent->contains("mcp_servers") &&
      raw_agent->at("mcp_servers").is_array()) {
    const boost::json::array& servers = raw_agent->at("mcp_servers").as_array();
    for (std::size_t i = 0; i < servers.size(); ++i) {
      if (!servers[i].is_object()) continue;
      WarnIgnoredKeys(servers[i].as_object(), config::ReadMcpServerKeys(),
                      entry.name + ".mcp_servers[" + std::to_string(i) + "]", warnings);
    }
  }
  return out.str();
}

}  // namespace

int RunCheck(const std::string& config_path, const std::filesystem::path& workspace_in) {
  const fs::path workspace = workspace_in.empty() ? fs::current_path() : workspace_in;

  std::string path = config_path;
  if (path.empty()) {
    // The same order `Initialize` reads a run by, resolved against this directory rather than
    // the process's: the file beside the directory `-C` named is the one the run would load.
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

  config::AgentEntry cfg;
  try {
    cfg = config::LoadAgentConfig(path);
  } catch (const std::exception& e) {
    // A file this build cannot read is the invocation being wrong: fixing it is the remedy.
    std::cerr << "cannot load " << path << ": " << e.what() << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }

  std::cout << "workspace  " << workspace.string() << "\n";
  std::cout << "agent      " << cfg.name << "\n\n";

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
  std::vector<std::string> warnings;
  // The file is its own raw entry: the object the report asks about fields of is the whole
  // file. A file that did not parse passes null rather than an empty object, so a line saying
  // nothing was declared is not printed as though it were a declaration.
  std::cout << FormatAgent(cfg, root.is_object() ? &root : nullptr, workspace, warnings) << "\n";

  // The session lives in the workspace under `.pu`, where the runtime reads it from, so
  // `--config` naming a file elsewhere does not move it. It outranks the file, so a name the
  // file does not configure is a fact to print rather than a file to refuse.
  const fs::path session_path = workspace / ".pu" / "session.json";
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
      const bool known = named == cfg.name;
      std::cout << "session  " << session_path.string() << "\n";
      if (known) {
        std::cout << "  names  " << named << "\n\n";
      } else {
        std::cout << "  names  " << named << ", which this file does not configure\n";
        std::cout << "  configured: " << cfg.name << "\n\n";
      }
    }
  }

  // Each target configures itself elsewhere, so the one delegation mistake a file can make is
  // asking itself: the turn that reaches it recurses until a budget stops it.
  std::vector<std::string> errors;
  for (const std::string& target : cfg.delegates) {
    if (target == cfg.name) errors.push_back(cfg.name + " delegates to itself");
  }
  if (!errors.empty()) {
    std::cout << "delegation\n";
    for (const std::string& error : errors) std::cout << "  " << error << "\n";
    std::cout << "\n";
  }

  if (!warnings.empty()) {
    std::cout << "warnings\n";
    for (const std::string& warning : warnings) std::cout << "  " << warning << "\n";
  }

  // A warning is not a failure: a file this build reads and an agent it can run is a pass, and
  // what it noticed is for a person to weigh.
  if (!errors.empty()) return static_cast<int>(ExitCode::kUsage);
  return static_cast<int>(ExitCode::kOk);
}

}  // namespace pu::cli
