// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <memory>
#include "pu/agent/agent_manager.hpp"
#include "pu/session/session.hpp"
#include "pu/core/fork_merge_service.hpp"

namespace pu {

class CommandRouter {
public:
  explicit CommandRouter(AgentManager& manager);

  bool Route(const std::string& input, Session& session, std::string& output);

private:
  ForkMergeService* GetOrCreateForkService(Session& session);

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
  std::unique_ptr<ForkMergeService> fork_service_;
  int max_depth_ = 5;
};

} // namespace pu