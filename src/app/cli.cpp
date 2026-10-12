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

void PrintChatHelp(Runtime& runtime) { std::cout << runtime.HelpText() << "\n"; }

void PrintNotice(const ExecutionResult& result) {
  if (result.notice.empty()) return;
  if (result.was_streamed) std::cout << "\n";
  std::cout << result.notice << "\n";
}

}  // namespace

CommandLine BuildCommandLine() {
  boost::program_options::options_description global("Global options");
  global.add_options()("help,h", "show help message")("version", "show version and exit")(
      "command", boost::program_options::value<std::string>(),
      "command to execute (ask/chat/serve)");

  // ask and chat name the same option, and program_options treats a long name
  // registered twice as ambiguous however it is grouped, so the two share one entry.
  boost::program_options::options_description session("ask and chat options");
  session.add_options()("agent", boost::program_options::value<std::string>(), "agent to use");

  boost::program_options::options_description ask("ask options");
  ask.add_options()("prompt", boost::program_options::value<std::string>(), "prompt to send");

  boost::program_options::options_description serve("serve options");
  serve.add_options()("host", boost::program_options::value<std::string>(),
                      "bind address (default 127.0.0.1)")(
      "port", boost::program_options::value<std::string>(), "port to listen on (default 8080)");

  boost::program_options::positional_options_description positional;
  positional.add("command", 1);
  positional.add("prompt", 1);

  CommandLine line;
  line.all.add(global).add(session).add(ask).add(serve);
  line.positional = std::move(positional);
  return line;
}

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
          PrintChatHelp(runtime);
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
