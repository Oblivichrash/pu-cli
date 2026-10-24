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

namespace pu::cli {

namespace {

namespace fs = std::filesystem;

// Everything ParseAgentEntry reads. A key outside this set is read by nothing, which is the
// one mistake a file cannot show by running: the field is simply not there any more.
const std::set<std::string>& ReadAgentKeys() {
  static const std::set<std::string> keys{"name",   "description",  "backend",
                                          "security", "mcp_servers", "tool_call_budget"};
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
    out << "  forbids      ";
    for (std::size_t i = 0; i < entry.security.forbidden_patterns.size(); ++i) {
      if (i > 0) out << ", ";
      out << "'" << entry.security.forbidden_patterns[i] << "'";
    }
    out << "\n";
  }
  if (!entry.security.approval_patterns.empty()) {
    out << "  approval     ";
    for (std::size_t i = 0; i < entry.security.approval_patterns.size(); ++i) {
      if (i > 0) out << ", ";
      out << "'" << entry.security.approval_patterns[i] << "'";
    }
    out << "   (no channel to ask on from the CLI: such a command is refused)\n";
  }
  if (!entry.mcp_servers.empty()) {
    out << "  mcp          ";
    for (std::size_t i = 0; i < entry.mcp_servers.size(); ++i) {
      if (i > 0) out << ", ";
      const config::McpServerConfig& server = entry.mcp_servers[i];
      out << server.name << " ("
          << (server.url.empty() ? server.command : server.url) << ")";
    }
    out << "\n";
  }
  if (entry.tool_call_budget) {
    out << "  tool budget  " << *entry.tool_call_budget << "\n";
  }

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
  return out.str();
}

}  // namespace

int RunCheck(const std::string& config_path) {
  std::string path = config_path;
  if (path.empty()) path = config::FindConfigPath();
  if (path.empty()) {
    std::cerr << "No agents.json found: looked in ./.pu/ and ~/.pu/.\n";
    return kUsage;
  }

  std::cout << "config  " << path << "\n\n";

  config::AgentsConfig cfg;
  try {
    cfg = config::LoadAgentsConfig(path);
  } catch (const std::exception& e) {
    // A file this build cannot read is the invocation being wrong: fixing it is the remedy.
    std::cerr << "cannot load " << path << ": " << e.what() << "\n";
    return kUsage;
  }

  const fs::path workspace = fs::current_path();
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

  if (!warnings.empty()) {
    std::cout << "warnings\n";
    for (const std::string& warning : warnings) std::cout << "  " << warning << "\n";
  }

  // A warning is not a failure: a file this build reads and an entry it can run is a pass,
  // and what it noticed is for a person to weigh.
  return kOk;
}

}  // namespace pu::cli
