// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli_ask.hpp"
#include "pu/cli_chat.hpp"
#include "pu/expert_factory.hpp"
#include "pu/renderer.hpp"
#include <curl/curl.h>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::atexit(curl_global_cleanup);

  pu::SetupSignalHandler();
  pu::expert::RegisterBuiltinFactories();

  if (argc < 2) {
    std::cerr << "Usage: pu <command> [options]\n"
              << "Commands:\n  ask    Send a single prompt to a model\n"
              << "  chat   Start interactive conversation\n";
    return 1;
  }
  std::string cmd = argv[1];
  if (cmd == "ask") return pu::cli::RunAskCommand(argc - 1, argv + 1);
  if (cmd == "chat") return pu::cli::RunChatCommand(argc - 1, argv + 1);
  std::cerr << "Unknown command: " << cmd << "\n";
  return 1;
}
