// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include "pu/agent.hpp"
#include "pu/core/platform.hpp"
#include "pu/build_config.hpp"
#include "pu/runtime.hpp"

#include <boost/program_options.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
  pu::platform::SetupSignalHandler();

  po::options_description global("Global options");
  global.add_options()("help,h", "show help message")("version", "show version and exit")(
      "command", po::value<std::string>(), "command to execute (ask/chat/serve)");

  po::options_description ask_opts("ask options");
  ask_opts.add_options()("agent", po::value<std::string>(), "agent to use")(
      "prompt", po::value<std::string>(), "prompt to send");

  po::options_description chat_opts("chat options");
  chat_opts.add_options()("agent", po::value<std::string>(), "agent to use");

  po::options_description serve_opts("serve options");
  serve_opts.add_options()("host", po::value<std::string>(), "bind address (default 127.0.0.1)")(
      "port", po::value<int>(), "port to listen on (default 8080)");

  po::positional_options_description pos;
  pos.add("command", 1);
  pos.add("prompt", 1);

  po::options_description all("Usage: pu <command> [options]");
  all.add(global).add(ask_opts).add(chat_opts).add(serve_opts);

  po::variables_map vm;
  try {
    po::store(po::command_line_parser(argc, argv).options(all).positional(pos).run(), vm);
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
      // Where to listen: the command line, then the environment, then the workspace's
      // own configuration, then the defaults.
      std::string host = "127.0.0.1";
      int port = 8080;
      bool host_given = false;
      bool port_given = false;

      if (vm.count("host")) {
        host = vm["host"].as<std::string>();
        host_given = true;
      } else if (const char* env = std::getenv("PU_SERVE_HOST"); env && *env != '\0') {
        host = env;
        host_given = true;
      }

      if (vm.count("port")) {
        port = vm["port"].as<int>();
        port_given = true;
      } else if (const char* env = std::getenv("PU_SERVE_PORT"); env && *env != '\0') {
        char* end = nullptr;
        long parsed = std::strtol(env, &end, 10);
        if (end && *end == '\0' && parsed >= 1 && parsed <= 65535) {
          port = static_cast<int>(parsed);
        } else {
          std::cerr << "Warning: invalid PU_SERVE_PORT '" << env << "', using default 8080\n";
        }
        // Asked for and refused, rather than asked for and overruled by the file.
        port_given = true;
      }

      // A directory is a session, and several are meant to be served side by side, so
      // the port a workspace answers on belongs in the workspace: otherwise every
      // shell that starts a server has to be told which one it is starting.
      if (!host_given || !port_given) {
        if (auto from_file = pu::config::FindServeOptions()) {
          if (!host_given && from_file->host) host = *from_file->host;
          if (!port_given && from_file->port) port = *from_file->port;
        }
      }

      return pu::cli::RunServe(host, port, runtime);
    }

    std::cerr << "Unknown command: " << cmd << "\n\n" << all << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << '\n';
    return 1;
  }
}
