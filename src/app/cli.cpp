// SPDX-License-Identifier: GPL-3.0-only
#include "pu/app/cli.hpp"

#include <exception>
#include <iostream>
#include <string>

#include <spdlog/spdlog.h>

#include "pu/config/agents.hpp"
#include "pu/command_router.hpp"
#include "pu/runtime.hpp"

namespace pu::cli {

namespace {

void PrintChatHelp() { std::cout << CommandRouter::GetHelpText() << "\n"; }

void PrintNotice(const ExecutionResult& result) {
  if (result.notice.empty()) return;
  if (result.was_streamed) std::cout << "\n";
  std::cout << result.notice << "\n";
}

}  // namespace

int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime) {
  try {
    if (!agent.empty()) runtime.SetDefaultAgent(agent);
    runtime.Initialize();

    bool is_command = false;
    ExecutionResult result = runtime.ProcessInput(prompt, is_command);
    if (result.has_error) {
      spdlog::error("{}", result.error_message.empty() ? "Request failed" : result.error_message);
    } else if (result.was_streamed) {
      std::cout << "\n";
    } else if (!result.content.empty()) {
      std::cout << result.content << "\n";
    } else if (!is_command) {
      std::cout << "\n";
    }
    PrintNotice(result);

    runtime.Shutdown();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }
  return 0;
}

int RunChat(const std::string& agent, Runtime& runtime) {
  try {
    if (!agent.empty()) runtime.SetDefaultAgent(agent);
    runtime.Initialize();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }

  const config::AgentEntry& active = runtime.ActiveAgent();
  std::string agent_info = "Connected to agent: " + active.name;
  if (!active.description.empty()) agent_info += " (" + active.description + ")";
  spdlog::info("{}", agent_info);
  spdlog::info("Type /help for available commands.");

  std::string input;
  while (std::cout << "> " << std::flush, std::getline(std::cin, input)) {
    if (input.empty()) continue;

    if (input == "/exit" || input == "/quit") break;

    try {
      bool is_command = false;
      ExecutionResult result = runtime.ProcessInput(input, is_command);
      if (result.has_error) {
        if (is_command && result.error_message.empty()) {
          std::cout << "Unknown command. ";
          PrintChatHelp();
        } else {
          spdlog::error("{}",
                        result.error_message.empty() ? "Processing failed" : result.error_message);
        }
      } else if (!result.was_streamed) {
        if (!result.content.empty()) {
          std::cout << result.content << "\n";
        } else if (!is_command) {
          std::cout << "\n";
        }
      } else {
      }

      PrintNotice(result);
    } catch (const std::exception& e) {
      spdlog::error("{}", e.what());
    }
  }

  runtime.Shutdown();
  std::cout << "\nGoodbye!\n";
  return 0;
}

}  // namespace pu::cli
