// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include "pu/agent_manager.hpp"
#include "pu/infra/platform.hpp"
#include "pu/runtime.hpp"

#include <boost/program_options.hpp>
#include <curl/curl.h>

#include <cstdlib>
#include <iostream>
#include <string>

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::atexit(curl_global_cleanup);

  pu::platform::SetupSignalHandler();

  po::options_description global("Global options");
  global.add_options()
    ("help,h", "show help message")
    ("command", po::value<std::string>(), "command to execute (ask/chat/serve)")
  ;

  po::options_description ask_opts("ask options");
  ask_opts.add_options()
    ("agent", po::value<std::string>(), "agent to use")
    ("prompt", po::value<std::string>(), "prompt to send")
  ;

  po::options_description chat_opts("chat options");
  chat_opts.add_options()
    ("agent", po::value<std::string>(), "agent to use")
  ;

  po::options_description serve_opts("serve options");
  serve_opts.add_options()
    ("host", po::value<std::string>(), "bind address (default 127.0.0.1)")
    ("port", po::value<int>(), "port to listen on (default 8080)")
  ;

  po::positional_options_description pos;
  pos.add("command", 1);
  pos.add("prompt", 1);

  po::options_description all("Usage: pu <command> [options]");
  all.add(global).add(ask_opts).add(chat_opts).add(serve_opts);

  po::variables_map vm;
  try {
    po::store(po::command_line_parser(argc, argv)
              .options(all)
              .positional(pos)
              .run(), vm);
    po::notify(vm);
  } catch (const po::error& e) {
    std::cerr << "Error: " << e.what() << "\n\n" << all << "\n";
    return 1;
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
      // Determine final host and port with fallback to environment and defaults
      std::string host = "127.0.0.1";
      int port = 8080;

      if (vm.count("host")) {
        host = vm["host"].as<std::string>();
      } else if (const char* env = std::getenv("PU_SERVE_HOST"); env && *env != '\0') {
        host = env;
      }

      if (vm.count("port")) {
        port = vm["port"].as<int>();
      } else if (const char* env = std::getenv("PU_SERVE_PORT"); env && *env != '\0') {
        char* end = nullptr;
        long parsed = std::strtol(env, &end, 10);
        if (end && *end == '\0' && parsed >= 1 && parsed <= 65535) {
          port = static_cast<int>(parsed);
        } else {
          std::cerr << "Warning: invalid PU_SERVE_PORT '" << env << "', using default 8080\n";
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
