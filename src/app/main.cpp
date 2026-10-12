// SPDX-License-Identifier: GPL-3.0-only
#include "pu/app/cli.hpp"

#include "pu/config/agents.hpp"
#include "pu/core/platform.hpp"
#include "pu/build_config.hpp"
#include "pu/runtime.hpp"

#include <boost/program_options.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
  pu::platform::SetupSignalHandler();

  const pu::cli::CommandLine line = pu::cli::BuildCommandLine();
  const po::options_description& all = line.all;

  po::variables_map vm;
  try {
    po::store(po::command_line_parser(argc, argv).options(all).positional(line.positional).run(), vm);
    po::notify(vm);
  } catch (const po::error& e) {
    std::cerr << "Error: " << e.what() << "\n\n" << all << "\n";
    return 1;
  }

  if (vm.count("version")) {
    std::cout << "pu " << PU_VERSION << "\n";
    return 0;
  }

  if (vm.count("help") || !vm.count("command")) {
    std::cout << all << "\n";
    return 0;
  }

  std::string cmd = vm["command"].as<std::string>();
  pu::Runtime runtime;

  try {
    if (cmd == "ask") {
      if (!vm.count("prompt")) {
        std::cerr << "Error: prompt required for ask command\n\n" << all << "\n";
        return 1;
      }
      std::string agent = vm.count("agent") ? vm["agent"].as<std::string>() : "";
      return pu::cli::RunAsk(agent, vm["prompt"].as<std::string>(), runtime);
    }

    if (cmd == "chat") {
      std::string agent = vm.count("agent") ? vm["agent"].as<std::string>() : "";
      return pu::cli::RunChat(agent, runtime);
    }

    if (cmd == "serve") {
      const auto named = [](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') return std::nullopt;
        return std::string(value);
      };
      const auto listen = pu::config::ResolveListenOptions(
          vm.count("host") ? std::optional(vm["host"].as<std::string>()) : std::nullopt,
          vm.count("port") ? std::optional(vm["port"].as<std::string>()) : std::nullopt,
          named("PU_SERVE_HOST"), named("PU_SERVE_PORT"), pu::config::FindServeOptions());
      return pu::cli::RunServe(listen.host, listen.port, runtime);
    }

    std::cerr << "Unknown command: " << cmd << "\n\n" << all << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << '\n';
    return 1;
  }
}
