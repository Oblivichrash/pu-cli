// SPDX-License-Identifier: GPL-3.0-only
#include "ui.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

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

void PrintAgents(const agent::config::AgentsConfig& cfg, const std::string& current) {
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

void PrintConversationList(const std::vector<pu::Conversation>& convs) {
  if (convs.empty()) {
    std::cout << "No saved conversations.\n";
    return;
  }
  for (const auto& c : convs) {
    std::cout << "  " << c.id << " (" << c.messages.size() << " messages) created: " << c.created_at << '\n';
  }
}

void PrintChatHelp() {
  std::cout << "Available commands:\n"
            << "  /help           Show this help\n"
            << "  /exit, /quit    Exit interactive mode\n"
            << "  /clear          Clear conversation history and agent lock\n"
            << "  /agent <name>   Switch to different agent\n"
            << "  /agents         List available agents\n"
            << "  /save [name] [--no-summary]  Save conversation and optionally generate summary\n"
            << "  /load <id>      Load a saved conversation\n"
            << "  /list           List saved conversations\n"
            << "  /export <id>    Export conversation to Markdown\n"
            << "  /note add <text>  Add a note for current agent\n"
            << "  /note show      Show notes for current agent\n"
            << "  /reload-tools   Reload external Python tools\n";
}

}  // namespace pu::cli
