// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "bash_expert.hpp"
#include "executor/command_executor.hpp"
#include <iostream>
#include <sstream>
#include <regex>

namespace pu::experts {

BashExpert::BashExpert()
    : executor_(std::make_unique<pu::executor::CommandExecutor>(".")) {}

void BashExpert::ResetSession() {
  // nothing
}

std::string BashExpert::GenerateCommand(const std::string& task,
                                        pu::expert::ExpertContext& ctx) {
  std::string prompt = "Generate a single bash command to accomplish this task. "
                       "Output ONLY the command surrounded by ```bash ... ```. "
                       "Task: " + task;
  std::string response = ctx.call_expert("chat", prompt);

  std::regex re(R"(```(?:bash)?\s*\n([\s\S]*?)\n```)");
  std::smatch match;
  if (std::regex_search(response, match, re)) {
    std::string cmd = match[1].str();
    cmd.erase(0, cmd.find_first_not_of(" \t\r\n"));
    cmd.erase(cmd.find_last_not_of(" \t\r\n") + 1);
    return cmd;
  }
  return response;
}

std::string BashExpert::ExecuteCommand(const std::string& command) {
  try {
    auto result = executor_->Execute(command);
    if (result.was_intercepted) {
      return "Command blocked: " + result.intercept_reason;
    }
    if (result.exit_code == 0) {
      return "Command executed successfully.\n" + result.stdout_content;
    } else {
      return "Command failed with exit code " + std::to_string(result.exit_code) +
             ".\n" + result.stderr_content + result.stdout_content;
    }
  } catch (const std::exception& e) {
    return std::string("Execution error: ") + e.what();
  }
}

std::string BashExpert::Handle(const std::string& input,
                               pu::expert::ExpertContext& ctx) {
  std::string command = GenerateCommand(input, ctx);
  if (command.empty()) {
    return "I couldn't generate a command for that request.";
  }

  std::ostringstream confirm_msg;
  confirm_msg << "I will execute the following command:\n\n  " << command << "\n\nProceed?";
  if (ctx.request_confirmation && !ctx.request_confirmation(confirm_msg.str())) {
    return "Command execution cancelled.";
  }

  return ExecuteCommand(command);
}

}  // namespace pu::experts
