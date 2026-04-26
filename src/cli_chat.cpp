// SPDX-License-Identifier: GPL-3.0-only

#include "pu/cli_chat.hpp"

#include "pu/backend.hpp"
#include "pu/expert.hpp"
#include "pu/expert_config.hpp"
#include "pu/http/http_client.hpp"
#include "pu/renderer.hpp"

#include "experts/chat/chat_expert.hpp"
#include "experts/bash/bash_expert.hpp"
#include "http/curl_http_client.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace pu::cli {

namespace {

void PrintHelp() {
  std::cout << "Available commands:\n"
            << "  /help           Show this help\n"
            << "  /exit, /quit    Exit interactive mode\n"
            << "  /clear          Clear conversation history and expert lock\n"
            << "  /expert <name>  Switch to different expert\n"
            << "  /experts        List available experts\n";
}

void PrintExperts(const pu::config::ExpertsConfig& config, const std::string& current) {
  std::cout << "Available experts:\n";
  for (const auto& entry : config.experts) {
    std::cout << "  " << entry.name;
    if (!entry.description.empty()) {
      std::cout << " - " << entry.description;
    }
    if (entry.name == current) {
      std::cout << " [current]";
    }
    std::cout << "\n";
  }
}

}  // namespace

int RunChatCommand(int argc, char* argv[]) {
  std::string initial_expert;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: pu chat [--expert <name>]\n";
      return 0;
    } else if (arg == "--expert") {
      if (i + 1 < argc) {
        initial_expert = argv[++i];
      } else {
        std::cerr << "Error: --expert requires an argument\n";
        return 1;
      }
    } else {
      std::cerr << "Error: unexpected argument '" << arg << "'\n";
      return 1;
    }
  }

  std::string config_path;
  try {
    config_path = pu::config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  pu::config::ExpertsConfig config;
  try {
    config = pu::config::LoadExpertsConfig(config_path);
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to load config: " << e.what() << "\n";
    return 1;
  }

  if (config.experts.empty()) {
    std::cerr << "Error: no experts configured\n";
    return 1;
  }

  std::string current_name = initial_expert.empty() ? config.default_expert : initial_expert;
  const pu::config::ExpertEntry* current_entry = nullptr;
  for (const auto& entry : config.experts) {
    if (entry.name == current_name) {
      current_entry = &entry;
      break;
    }
  }
  if (!current_entry) {
    std::cerr << "Error: expert '" << current_name << "' not found\n";
    return 1;
  }

  auto chat_http = std::make_unique<pu::http::CurlHttpClient>();
  std::unique_ptr<pu::backend::Backend> chat_backend;
  try {
    chat_backend = pu::config::CreateBackend(current_entry->backend, std::move(chat_http));
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to create backend: " << e.what() << "\n";
    return 1;
  }

  auto router_http = std::make_unique<pu::http::CurlHttpClient>();
  std::unique_ptr<pu::backend::Backend> router_backend;
  try {
    router_backend = pu::config::CreateBackend(current_entry->backend, std::move(router_http));
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to create router backend: " << e.what() << "\n";
    return 1;
  }

  pu::backend::Backend* router_raw = router_backend.get();
  pu::expert::ExpertManager manager(std::move(router_backend));
  manager.RegisterExpert(
      std::make_unique<pu::experts::ChatExpert>(std::move(chat_backend), current_entry->name));
  manager.RegisterExpert(
      std::make_unique<pu::experts::BashExpert>(*router_raw,
                                                std::make_unique<pu::executor::CommandExecutor>(".")));

  if (!initial_expert.empty()) {
    manager.SetActiveExpert(initial_expert);
  }

  std::cout << "[INFO] Connected to expert: " << current_entry->name;
  if (!current_entry->description.empty()) {
    std::cout << " (" << current_entry->description << ")";
  }
  std::cout << "\nType /help for available commands.\n\n";

  std::string input;
  while (true) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, input)) {
      break;
    }
    if (input.empty()) {
      continue;
    }

    if (input[0] == '/') {
      if (input == "/help") {
        PrintHelp();
      } else if (input == "/exit" || input == "/quit") {
        break;
      } else if (input == "/clear") {
        manager.ClearSessions();
        std::cout << "[INFO] Conversation history and expert lock cleared.\n";
      } else if (input == "/experts") {
        PrintExperts(config, manager.GetActiveExpert());
      } else if (input.rfind("/expert ", 0) == 0) {
        std::string new_name = input.substr(8);
        size_t start = new_name.find_first_not_of(" \t");
        if (start != std::string::npos) {
          new_name = new_name.substr(start);
        } else {
          new_name.clear();
        }
        size_t end = new_name.find_last_not_of(" \t");
        if (end != std::string::npos) {
          new_name = new_name.substr(0, end + 1);
        } else {
          new_name.clear();
        }
        if (new_name.empty()) {
          std::cerr << "Error: expert name required.\n";
          continue;
        }

        const pu::config::ExpertEntry* new_entry = nullptr;
        for (const auto& entry : config.experts) {
          if (entry.name == new_name) {
            new_entry = &entry;
            break;
          }
        }
        if (!new_entry) {
          std::cerr << "Error: expert '" << new_name << "' not found.\n";
          continue;
        }

        manager.SetActiveExpert(new_entry->name);
        std::cout << "[INFO] Switched to expert: " << new_entry->name;
        if (!new_entry->description.empty()) {
          std::cout << " (" << new_entry->description << ")";
        }
        std::cout << "\n";
      } else {
        std::cerr << "Unknown command: " << input << "\nType /help for available commands.\n";
      }
      continue;
    }

    try {
      manager.Dispatch(input);
    } catch (const std::exception& e) {
      std::cerr << "\nError: " << e.what() << "\n\n";
    }
  }

  std::cout << "\nGoodbye!\n";
  return 0;
}

}  // namespace pu::cli
