// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli_ask.hpp"
#include "pu/cli_app_setup.hpp"
#include "pu/agent.hpp"
#include <iostream>
#include <string>

namespace pu::cli {

namespace {
void PrintUsage() {
  std::cerr << "Usage: pu ask [--expert <name>] [--show-reasoning] <prompt>\n"
            << "Options:\n"
            << "  --expert <name>          Specify the expert to use\n"
            << "  --show-reasoning         Show model's internal reasoning\n"
            << "  -h, --help               Show this help message\n";
}
}  // namespace

int RunAskCommand(int argc, char* argv[]) {
  std::string requested_expert;
  std::string prompt;
  bool show_reasoning = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") { PrintUsage(); return 0; }
    else if (arg == "--expert") {
      if (i + 1 < argc) requested_expert = argv[++i];
      else { std::cerr << "Error: --expert requires an argument\n"; PrintUsage(); return 1; }
    } else if (arg == "--show-reasoning") { show_reasoning = true; }
    else if (prompt.empty()) { prompt = arg; }
    else { std::cerr << "Error: unexpected argument '" << arg << "'\n"; PrintUsage(); return 1; }
  }

  if (prompt.empty()) { std::cerr << "Error: prompt is required\n"; PrintUsage(); return 1; }

  auto ctx = SetupAppContext(requested_expert, show_reasoning);
  try {
    ctx.manager.Dispatch(prompt);
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << "\n";
    return 1;
  }
  return 0;
}

}  // namespace pu::cli
