// SPDX-License-Identifier: GPL-3.0-only
#include "pu/app/cli.hpp"

#include "pu/config/agents.hpp"
#include "pu/core/platform.hpp"
#include "pu/build_config.hpp"
#include "pu/runtime/runtime.hpp"

#include <boost/program_options.hpp>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace po = boost::program_options;

namespace {

std::string OptionOr(const po::variables_map& vm, const char* name, const std::string& fallback) {
  return vm.count(name) ? vm[name].as<std::string>() : fallback;
}

std::optional<std::string> OptionalOption(const po::variables_map& vm, const char* name) {
  if (!vm.count(name)) return std::nullopt;
  return vm[name].as<std::string>();
}

std::optional<std::string> EnvOr(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return std::nullopt;
  return std::string(value);
}

}  // namespace

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
      return pu::cli::RunAsk(OptionOr(vm, "agent", ""), vm["prompt"].as<std::string>(), runtime);
    }

    if (cmd == "chat") {
      return pu::cli::RunChat(OptionOr(vm, "agent", ""), runtime);
    }

    if (cmd == "models") {
      return pu::cli::RunModels(OptionOr(vm, "agent", ""), vm.count("verify") > 0, runtime);
    }

    if (cmd == "probe") {
      return pu::cli::RunProbe(OptionOr(vm, "agent", ""), OptionOr(vm, "prompt", "Say the single word: pong"),
                               OptionOr(vm, "out", "./provider-probes"), vm.count("no-save") == 0,
                               runtime);
    }

    if (cmd == "serve") {
      const auto listen = pu::config::ResolveListenOptions(
          OptionalOption(vm, "host"), OptionalOption(vm, "port"), EnvOr("PU_SERVE_HOST"),
          EnvOr("PU_SERVE_PORT"), pu::config::FindServeOptions());
      return pu::cli::RunServe(listen.host, listen.port, runtime);
    }

    std::cerr << "Unknown command: " << cmd << "\n\n" << all << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << '\n';
    return 1;
  }
}
