// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"
#include "common.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace pu::cli {

int RunLearn(int argc, char* argv[]) {
  double threshold = 0.6;
  int max_sessions = 10;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cerr << "Usage: pu learn [--threshold <0.0-1.0>] [--max-sessions <N>]\n"
                << "  Analyze successful conversations and generate new agent definitions.\n"
                << "  Generated agents are saved to ~/.pu/generated/agents/\n";
      return 0;
    } else if (arg == "--threshold") {
      if (i + 1 < argc) {
        threshold = std::stod(argv[++i]);
      } else {
        std::cerr << "Error: --threshold requires a value\n";
        return 1;
      }
    } else if (arg == "--max-sessions") {
      if (i + 1 < argc) {
        max_sessions = std::stoi(argv[++i]);
      } else {
        std::cerr << "Error: --max-sessions requires a number\n";
        return 1;
      }
    } else {
      std::cerr << "Error: unknown argument '" << arg << "'\n";
      return 1;
    }
  }

  const char* home = std::getenv("HOME");
  auto pu_dir = std::filesystem::path(home ? home : ".") / ".pu";
  auto conv_dir = pu_dir / "conversations";
  auto generated_dir = pu_dir / "generated" / "agents";

  if (!std::filesystem::exists(conv_dir)) {
    std::cerr << "No conversations found in " << conv_dir << "\n";
    return 0;
  }

  std::filesystem::create_directories(generated_dir);

  // Placeholder: iterate conversations, analyze, generate agents.
  std::cout << "[Learn] Scanning " << conv_dir << " for sessions (threshold=" << threshold
            << ", max=" << max_sessions << ")\n";
  std::cout << "[Learn] Generated agents will be saved to " << generated_dir << "\n";
  std::cout << "[Learn] (Implementation in progress)\n";

  return 0;
}

} // namespace pu::cli
