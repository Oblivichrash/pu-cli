// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include "pu/agent_core.hpp"
#include "pu/session/session.hpp"

namespace pu {

class CommandRouter {
public:
  explicit CommandRouter(agent::AgentManager& manager);

  bool Route(const std::string& input, Session& session, std::string& output);

private:
  bool HandleHelp(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleFork(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleMerge(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleBackend(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleAgent(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleAgents(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleSave(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleLoad(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleList(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleExport(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleNote(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleClear(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleReloadTools(const std::vector<std::string>& args, Session& session, std::string& output);
  bool HandleStack(const std::vector<std::string>& args, Session& session, std::string& output);

  agent::AgentManager& manager_;
  int max_depth_ = 5;
};

} // namespace pu
