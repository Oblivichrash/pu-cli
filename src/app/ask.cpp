// SPDX-License-Identifier: GPL-3.0-only
//
// `pu ask` and `pu chat`: the two commands that take a turn in the foreground. They share a
// file because they are the same act — a prompt in, a turn out — and differ only in whether
// the conversation outlives the prompt: `ask` settles the session before the turn, so the next
// question inherits nothing, and `chat` keeps it, so the next line continues the first.
//
#include "pu/app/cli.hpp"

#include <exception>
#include <iostream>
#include <string>

#include <spdlog/spdlog.h>

#include "pu/core/platform.hpp"
#include "pu/runtime/runtime.hpp"

#include "app/cli_output.hpp"

namespace pu::cli {

int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime,
           bool show_usage) {
  // The prompt is the whole of what this turn knows. `chat` and `serve` remember, and are
  // where a conversation that continues belongs; an answer asked for once should not move the
  // conversation the next `pu ask` inherits.
  return WithInitializedRuntime(
      agent, runtime,
      [&] {
        ExecutionResult result =
            runtime.ProcessInput(prompt, platform::ShutdownToken(), nullptr, {},
                                 nullptr, platform::ShutdownToken());
        detail::PrintFailure(result);
        if (!result.has_error) {
          if (result.was_streamed) {
            std::cout << "\n";
          } else if (!result.content.empty()) {
            std::cout << result.content << "\n";
          } else if (!result.was_command) {
            std::cout << "\n";
          }
        }
        detail::PrintNotice(result);
        if (show_usage) detail::PrintUsage(result);
        return static_cast<int>(detail::ExitCodeForAsk(result));
      },
      /*ephemeral=*/true);
}

int RunChat(const std::string& agent, Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
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
        ExecutionResult result =
            runtime.ProcessInput(input, platform::ShutdownToken(), nullptr, {},
                                 nullptr, platform::ShutdownToken());
        if (result.has_error) {
          detail::PrintFailure(result);
        } else if (!result.was_streamed) {
          if (!result.content.empty()) {
            std::cout << result.content << "\n";
          } else if (!result.was_command) {
            std::cout << "\n";
          }
        }

        detail::PrintNotice(result);
      } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
      }
    }
    std::cout << "\nGoodbye!\n";
    return static_cast<int>(ExitCode::kOk);
  });
}
}  // namespace pu::cli
