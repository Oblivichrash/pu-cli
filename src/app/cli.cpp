// SPDX-License-Identifier: GPL-3.0-only
#include "pu/app/cli.hpp"

#include <exception>
#include <functional>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "pu/config/agents.hpp"
#include "pu/config/backend.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime/command_router.hpp"
#include "pu/llm/model_listing.hpp"
#include "pu/llm/probe.hpp"
#include "pu/runtime/runtime.hpp"

namespace pu::cli {

namespace {

void PrintChatHelp(Runtime& runtime) { std::cout << runtime.HelpText() << "\n"; }

void PrintNotice(const ExecutionResult& result) {
  if (result.notice.empty()) return;
  if (result.was_streamed) std::cout << "\n";
  std::cout << result.notice << "\n";
}

int WithInitializedRuntime(const std::string& agent, Runtime& runtime,
                           const std::function<void()>& body) {
  try {
    if (!agent.empty()) runtime.SetDefaultAgent(agent);
    runtime.Initialize();
    body();
    runtime.Shutdown();
  } catch (const std::exception& e) {
    spdlog::error("{}", e.what());
    return 1;
  }
  return 0;
}

}  // namespace

CommandLine BuildCommandLine() {
  boost::program_options::options_description global("Global options");
  global.add_options()("help,h", "show help message")("version", "show version and exit")(
      "command", boost::program_options::value<std::string>(),
      "command to execute (ask/chat/models/probe/serve)");

  boost::program_options::options_description session("ask, chat and models options");
  session.add_options()("agent", boost::program_options::value<std::string>(), "agent to use");

  boost::program_options::options_description ask("ask options");
  ask.add_options()("prompt", boost::program_options::value<std::string>(), "prompt to send");

  boost::program_options::options_description models("models options");
  models.add_options()("verify", "ask each listed model for one token and report what answered");

  boost::program_options::options_description probe("probe options");
  probe.add_options()("out", boost::program_options::value<std::string>(),
                      "directory the raw stream is written to (default ./provider-probes)")(
      "no-save", "print the summary without writing the raw stream");

  boost::program_options::options_description serve("serve options");
  serve.add_options()("host", boost::program_options::value<std::string>(),
                      "bind address (default 127.0.0.1)")(
      "port", boost::program_options::value<std::string>(), "port to listen on (default 8080)");

  boost::program_options::positional_options_description positional;
  positional.add("command", 1);
  positional.add("prompt", 1);

  CommandLine line;
  line.all.add(global).add(session).add(ask).add(models).add(probe).add(serve);
  line.positional = std::move(positional);
  return line;
}

int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    bool is_command = false;
    ExecutionResult result =
        runtime.ProcessInput(prompt, is_command, platform::ShutdownToken(), nullptr, {}, nullptr,
                             platform::ShutdownToken());
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
  });
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
        bool is_command = false;
        ExecutionResult result =
            runtime.ProcessInput(input, is_command, platform::ShutdownToken(), nullptr, {},
                                 nullptr, platform::ShutdownToken());
        if (result.has_error) {
          if (is_command && result.error_message.empty()) {
            std::cout << "Unknown command. ";
            PrintChatHelp(runtime);
          } else {
            spdlog::error("{}\n", result.error_message.empty() ? "Processing failed"
                                                               : result.error_message);
          }
        } else if (!result.was_streamed) {
          if (!result.content.empty()) {
            std::cout << result.content << "\n";
          } else if (!is_command) {
            std::cout << "\n";
          }
        }

        PrintNotice(result);
      } catch (const std::exception& e) {
        spdlog::error("{}", e.what());
      }
    }
    std::cout << "\nGoodbye!\n";
  });
}

// SPDX-License-Identifier: GPL-3.0-only
// One probe run: a single-token prompt to the agent's backend, the raw stream kept,
// and the field names the gateway actually sent read back out of it. A ResponseShape
// for a new gateway is written from this capture rather than from a specification.
int RunProbe(const std::string& agent, const std::string& prompt, const std::string& out_dir,
             bool save, Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    const config::Backend backend = runtime.CurrentBackend();

    std::optional<std::string> raw_path;
    if (save) {
      std::filesystem::create_directories(out_dir);
      raw_path = (std::filesystem::path(out_dir) /
                  (std::string(config::GatewayName(backend.gateway)) + ".raw"))
                     .string();
      std::cout << "raw stream: " << *raw_path << "\n";
    }

    const llm::ProbeReport report = llm::ProbeStream(backend, runtime.Http(), prompt, raw_path);
    std::cout << "\n" << llm::FormatProbeReport(report) << "\n";
  });
}

int RunModels(const std::string& agent, bool verify, Runtime& runtime) {

  return WithInitializedRuntime(agent, runtime, [&] {
    const config::Backend backend = runtime.CurrentBackend();
    const std::vector<llm::ModelEntry> models = config::ListModels(backend, runtime.Http());

    if (!verify) {
      std::cout << llm::FormatModelList(models, config::GatewayName(backend.gateway),
                                        backend.host, backend.model)
                << "\n";
      return;
    }

    const std::vector<llm::ModelCheck> checks =
        llm::VerifyModels(backend, models, runtime.SharedHttp());
    int answered = 0;
    for (const llm::ModelCheck& check : checks) {
      std::cout << check.model << "  ";
      if (check.answered) {
        std::cout << "ok";
        ++answered;
      } else {
        std::cout << "unavailable: " << check.detail;
      }
      std::cout << "\n";
    }
    std::cout << answered << " of " << checks.size() << " model(s) answered\n";
  });
}

}  // namespace pu::cli
