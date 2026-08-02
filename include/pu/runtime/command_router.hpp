// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "pu/agent/agent_manager.hpp"
#include "pu/session/session.hpp"
#include "pu/storage/session_store.hpp"

namespace pu {

class Runtime;

class CommandRouter {
public:
  using CommandHandler =
      bool (CommandRouter::*)(const std::vector<std::string>&, Session&, std::string&);

  CommandRouter(AgentManager& manager, Runtime& runtime);

  bool Route(const std::string& input, Session& session, std::string& output);

  static std::string GetHelpText();

private:
  struct CommandEntry {
    CommandHandler handler;
    std::string help;
  };

  struct Registry {
    std::unordered_map<std::string, CommandEntry> commands;
    std::vector<std::string> order;
  };

  static Registry BuildRegistry();
  static const Registry kRegistry;

  bool RequireMinArgs(const std::vector<std::string>& args, size_t min,
                      const std::string& usage, std::string& output) const;

  std::string FormatUsage(const std::string& cmd, const std::string& usage) const;

  SessionStore GetSessionStore() const;

  bool HandleHelp(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleBackend(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleAgents(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleSave(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleLoad(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleList(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleExport(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleNote(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleClear(const std::vector<std::string>& args, Session& session, std::string& output);

  AgentManager& manager_;
  Runtime& runtime_;
};

} // namespace pu
