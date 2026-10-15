// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "pu/config/backend.hpp"
#include "pu/session/session.hpp"

namespace pu {

class Runtime;

class CommandRouter {
 public:
  explicit CommandRouter(Runtime& runtime);

  bool Route(const std::string& input, Session& session, std::string& output);

  std::string Help();

 private:
  using Args = std::span<const std::string>;
  using Output = std::string&;

  struct Command {
    std::string help;
    std::function<bool(Args, Session&, Output)> run;
  };

  static const std::vector<std::string>& CommandOrder();

  bool RequireMinArgs(Args args, size_t min, const std::string& usage, Output output) const;
  std::string FormatUsage(const std::string& cmd, const std::string& usage) const;

  bool HandleBackend(Args args, Output output);
  bool HandleModels(Args args, Output output);
  bool ReportModels(const config::BackendConfig& cfg, Output output);
  bool HandleAgents(Args args, Session& session, Output output);
  bool HandleRewind(Args args, Session& session, Output output);
  bool HandleThinking(Args args, Output output);

  Runtime& runtime_;
  std::unordered_map<std::string, Command> commands_;
};

}  // namespace pu