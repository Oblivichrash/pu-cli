// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <memory>

#include "pu/agent/agent_manager.hpp"
#include "pu/core/fork_merge_service.hpp"
#include "pu/session/session.hpp"
#include "pu/storage/session_store.hpp"

namespace pu {

class Runtime;

class CommandRouter {
public:
  CommandRouter(AgentManager& manager, Runtime& runtime,
                std::shared_ptr<ForkMergeService> fork_service);

  bool Route(const std::string& input, Session& session, std::string& output);

  ForkMergeService* GetForkService() const { return fork_service_.get(); }

private:
  bool RequireMinArgs(const std::vector<std::string>& args, size_t min,
                      const std::string& usage, std::string& output) const;

  std::string FormatUsage(const std::string& cmd, const std::string& usage) const;

  SessionStore GetSessionStore() const;

  bool HandleHelp(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleFork(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleMerge(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleBackend(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleAgents(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleSave(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleLoad(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleList(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleExport(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleNote(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleClear(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleReloadTools(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleStack(const std::vector<std::string>& args, Session& session, std::string& output);

  AgentManager& manager_;
  Runtime& runtime_;
  std::shared_ptr<ForkMergeService> fork_service_;
};

} // namespace pu
