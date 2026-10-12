// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pu/session/session.hpp"

namespace pu {

class Runtime;

class CommandRouter {
 public:
  explicit CommandRouter(Runtime& runtime);

  bool Route(const std::string& input, Session& session, std::string& output);

  std::string GetHelpText();

 private:
  using Args = const std::vector<std::string>&;
  using Output = std::string&;

  struct Command {
    std::string help;
    std::function<bool(Args, Session&, Output)> run;
  };

  static const std::vector<std::string>& CommandOrder();
  const std::unordered_map<std::string, Command>& Commands();

  bool RequireMinArgs(Args args, size_t min, const std::string& usage, Output output) const;

  std::string FormatUsage(const std::string& cmd, const std::string& usage) const;

  bool HandleBackend(Args args, Output output);
  bool HandleAgents(Args args, Session& session, Output output);
  bool HandleRewind(Args args, Session& session, Output output);
  bool HandleThinking(Args args, Output output);

  Runtime& runtime_;
};

}  // namespace pu