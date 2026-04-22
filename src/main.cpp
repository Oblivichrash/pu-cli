#include "pu/cli_ask.hpp"

#include <curl/curl.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::atexit(curl_global_cleanup);

  if (argc < 2) {
    std::cerr << "Usage: pu <command> [options]\n"
              << "Commands:\n"
              << "  ask   Send a single prompt to a model\n";
    return 1;
  }

  std::string cmd = argv[1];
  if (cmd == "ask") {
    return pu::cli::RunAskCommand(argc - 1, argv + 1);
  } else {
    std::cerr << "Unknown command: " << cmd << "\n";
    return 1;
  }
}
