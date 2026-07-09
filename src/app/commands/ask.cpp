// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"
#include "common.hpp"
#include <cstdlib>
#include <iostream>

namespace pu::cli {

int RunAsk(int argc, char* argv[]) {
  std::string requested_agent;
  std::string prompt;
  bool show_reasoning = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cerr << "Usage: pu ask [--agent <name>] [--show-reasoning] <prompt>\n"
                << "Options:\n"
                << "  --agent <name>          Specify the agent to use\n"
                << "  --show-reasoning        Show model's internal reasoning\n"
                << "  -h, --help              Show this help message\n";
      return 0;
    } else if (arg == "--agent") {
      if (i + 1 < argc) requested_agent = argv[++i];
      else { std::cerr << "Error: --agent requires an argument\n"; return 1; }
    } else if (arg == "--show-reasoning") {
      show_reasoning = true;
    } else if (prompt.empty()) {
      prompt = arg;
    } else {
      std::cerr << "Error: unexpected argument '" << arg << "'\n";
      return 1;
    }
  }

  if (prompt.empty()) {
    std::cerr << "Error: prompt is required\n";
    return 1;
  }

  auto ctx = SetupAppContext(requested_agent, show_reasoning);
  try {
    ctx.manager.Dispatch(prompt);
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << "\n";
    return 1;
  }
  return 0;
}

} // namespace pu::cli
