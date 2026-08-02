// SPDX-License-Identifier: GPL-3.0-only
#include "ui.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "pu/runtime/command_router.hpp"

namespace pu::cli {

std::string Trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t");
  if (start == std::string::npos) return {};
  auto end = s.find_last_not_of(" \t");
  return s.substr(start, end - start + 1);
}

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string GenerateId() {
  auto now = std::chrono::high_resolution_clock::now();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  std::ostringstream ss;
  ss << std::hex << nanos;
  return "conv-" + ss.str();
}

void PrintAgents(const config::AgentsConfig& cfg, const std::string& current) {
  std::cout << "Available agents:\n";
  for (const auto& entry : cfg.agents) {
    std::cout << "  " << entry.name;
    if (!entry.description.empty()) {
      std::cout << " - " << entry.description;
    }
    if (entry.name == current) {
      std::cout << " [current]";
    }
    std::cout << '\n';
  }
}

void PrintChatHelp() {
  std::cout << CommandRouter::GetHelpText() << "\n";
}

}  // namespace pu::cli
