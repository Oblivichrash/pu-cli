// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include "pu/agent/agent_manager.hpp"
#include "pu/infra/platform.hpp"
#include "pu/runtime/runtime.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::atexit(curl_global_cleanup);

  pu::platform::SetupSignalHandler();

  if (argc < 2) {
    std::cerr << "Usage: pu <command> [options]\n"
              << "Commands:\n  ask    Send a single prompt to a model\n"
              << "  chat   Start interactive conversation\n";
    return 1;
  }
  std::string cmd = argv[1];

  pu::Runtime runtime;

  try {
    if (cmd == "ask") return pu::cli::RunAsk(argc - 1, argv + 1, runtime);
    if (cmd == "chat") return pu::cli::RunChat(argc - 1, argv + 1, runtime);
    std::cerr << "Unknown command: " << cmd << '\n';
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << '\n';
    return 1;
  }
}
