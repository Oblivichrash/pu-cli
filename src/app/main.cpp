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
#include <vector>
#include <sstream>
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
  // Where the subcommand's name is, so a failure can show that command's page: a flag that
  // belongs to `models` is not a mistake on the `ask` page, and printing the whole line for
  // every error is how a reader learns nothing about the command they typed.
  std::string named_command;
  try {
    const po::parsed_options parsed =
        po::command_line_parser(argc, argv).options(all).positional(line.positional).run();
    po::store(parsed, vm);
    po::notify(vm);

    for (const po::option& option : parsed.options) {
      if (option.string_key == "command" && !option.value.empty()) {
        named_command = option.value.front();
        break;
      }
    }
    // The positional was never matched, so the parser reported it as an argument with no
    // name. Its value is still the command the rest of the line is about.
    if (named_command.empty()) {
      for (const po::option& option : parsed.options) {
        if (option.string_key.empty() && !option.value.empty()) {
          named_command = option.value.front();
          break;
        }
      }
    }
  } catch (const po::error& e) {
    std::cerr << "Error: " << e.what() << "\n\n" << line.HelpFor(named_command) << "\n";
    return 1;
  }

  if (vm.count("version")) {
    std::cout << "pu " << PU_VERSION << "\n";
    return 0;
  }

  if (vm.count("help")) {
    // `pu models --help` is a question about models, and `pu --help` about the line.
    std::cout << line.HelpFor(vm.count("command") ? vm["command"].as<std::string>() : "") << "\n";
    return 0;
  }

  if (!vm.count("command")) {
    std::cout << all << "\n";
    return 0;
  }

  std::string cmd = vm["command"].as<std::string>();

  // The parser had to accept every name to learn which command was typed; now that it is
  // known, each flag in it is held against the page for it. A flag that belongs to another
  // command is what it always was — an option this one does not have — and a typo is caught
  // before a turn runs rather than silently ignored.
  //
  // Only flags are checked, and only the ones `all` accepted: a positional value is not an
  // option, and asking boost to judge the whole line again would call the prompt one.
  {
    std::vector<std::string> rejected;
    for (int i = 1; i < argc; ++i) {
      const std::string token = argv[i];
      if (token.size() < 2 || token[0] != '-' || token == "--") continue;
      if (token.rfind("-h", 0) == 0 || token.rfind("--help", 0) == 0) continue;
      // `--name` or `--name=value`, and the page knows the name it abbreviates.
      const std::string name = token.substr(0, token.find('='));
      // What an option calls itself is its name without the leading dashes: `--show-usage`
      // is declared `show-usage`, `-h` as `help`. Stopping at a hyphen inside the name would
      // refuse every flag that has one, so only the leading run is removed.
      const std::size_t first = name.find_first_not_of('-');
      const std::string bare = first == std::string::npos ? std::string() : name.substr(first);
      if (line.HelpFor(cmd).find_nothrow(bare, /*approx=*/false) != nullptr) continue;
      if (line.HelpFor(cmd).find_nothrow(name, /*approx=*/false) != nullptr) continue;
      if (line.HelpFor(cmd).find_nothrow(bare, /*approx=*/true) != nullptr) continue;
      rejected.push_back(name);
    }
    if (!rejected.empty()) {
      std::cerr << "Error: " << rejected.front() << " is not an option of '" << cmd << "'\n\n"
                << line.HelpFor(cmd) << "\n";
      return pu::cli::kUsage;
    }
  }

  pu::Runtime runtime;

  try {
    if (cmd == "ask") {
      // An empty prompt is a mistake in the invocation, not a turn to send: the store would
      // gain a question with no words and the provider a request for nothing.
      if (!vm.count("prompt") || vm["prompt"].as<std::string>().empty()) {
        std::cerr << "Error: prompt required for ask command\n\n" << all << "\n";
        return pu::cli::kUsage;
      }
      return pu::cli::RunAsk(OptionOr(vm, "agent", ""), vm["prompt"].as<std::string>(), runtime,
                             vm.count("show-usage") > 0);
    }

    if (cmd == "chat") {
      return pu::cli::RunChat(OptionOr(vm, "agent", ""), runtime);
    }

    if (cmd == "models") {
      return pu::cli::RunModels(OptionOr(vm, "agent", ""), OptionOr(vm, "gateway", ""),
                               OptionOr(vm, "model", ""), vm.count("verify") > 0,
                               OptionOr(vm, "entry", ""), vm.count("caps") > 0, runtime);
    }

    if (cmd == "agents") {
      return pu::cli::WithInitializedRuntime(OptionOr(vm, "agent", ""), runtime, [&] {
        return pu::cli::RunAgents(runtime, vm.count("json") > 0);
      });
    }

    if (cmd == "sse") {
      pu::cli::SseOptions options;
      // The URL is positional or named; a caller reading a feed writes it once and types the
      // rest as flags.
      options.url = OptionOr(vm, "url", vm.count("command") > 0 ? OptionOr(vm, "prompt", "") : "");
      if (options.url.empty()) {
        std::cerr << "Error: no URL to read\n\n" << line.HelpFor(cmd) << "\n";
        return pu::cli::kUsage;
      }
      if (vm.count("header") > 0) {
        options.headers = vm["header"].as<std::vector<std::string>>();
      }
      if (vm.count("body") > 0) {
        const std::string given = vm["body"].as<std::string>();
        if (given == "-") {
          std::ostringstream body;
          body << std::cin.rdbuf();
          options.body = body.str();
        } else {
          options.body = given;
        }
      }
      options.method = vm.count("post") > 0 ? pu::http::HttpMethod::kPost
                                            : pu::http::HttpMethod::kGet;
      options.as_json = vm.count("json") > 0;
      options.quiet = vm.count("quiet") > 0;
      options.raw = vm.count("raw") > 0;

      return pu::cli::WithInitializedRuntime("", runtime,
                                             [&] { return pu::cli::RunSse(options, runtime); });
    }

    if (cmd == "check") {
      // No Runtime: the point is to read a file, and a file this build cannot load is what
      // it is there to report rather than to fail on halfway through.
      pu::Runtime unused;
      return pu::cli::RunCheck(OptionOr(vm, "config", ""), unused);
    }

    if (cmd == "probe") {
      return pu::cli::RunProbe(OptionOr(vm, "agent", ""), OptionOr(vm, "gateway", ""),
                               OptionOr(vm, "model", ""),
                               OptionOr(vm, "prompt", "Say the single word: pong"),
                               OptionOr(vm, "out", "./provider-probes"), vm.count("no-save") == 0,
                               vm.count("listing") > 0, runtime);
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
