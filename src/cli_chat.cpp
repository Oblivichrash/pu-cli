// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "pu/cli_chat.hpp"

#include "http/curl_http_client.hpp"
#include "pu/backend.hpp"
#include "pu/http/http_client.hpp"
#include "pu/model_config.hpp"
#include "pu/renderer.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace pu::cli {

namespace {

void PrintHelp() {
  std::cout << "Available commands:\n"
            << "  /help           Show this help\n"
            << "  /exit, /quit    Exit interactive mode\n"
            << "  /clear          Clear conversation history\n"
            << "  /model <name>   Switch to a different model (clears history)\n"
            << "  /models         List available models\n";
}

void PrintModels(const pu::config::ModelsFile& models, const std::string& current) {
  std::cout << "Available models:\n";
  for (const auto& entry : models.models) {
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
  std::string initial_model;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: pu chat [-m <model>]\n";
      return 0;
    } else if (arg == "-m" || arg == "--model") {
      if (i + 1 < argc) {
        initial_model = argv[++i];
      } else {
        std::cerr << "Error: --model requires an argument\n";
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

  pu::config::ModelsFile models;
  try {
    models = pu::config::LoadModelsConfig(config_path);
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to load config: " << e.what() << "\n";
    return 1;
  }

  if (models.models.empty()) {
    std::cerr << "Error: no models configured\n";
    return 1;
  }

  const pu::config::ModelEntry* current_entry = nullptr;
  std::string current_name = initial_model.empty() ? models.default_model : initial_model;
  for (const auto& entry : models.models) {
    if (entry.name == current_name) {
      current_entry = &entry;
      break;
    }
  }
  if (!current_entry) {
    std::cerr << "Error: model '" << current_name << "' not found\n";
    return 1;
  }

  auto http = std::make_unique<pu::http::CurlHttpClient>();
  std::unique_ptr<pu::backend::Backend> backend;
  try {
    backend = pu::config::CreateBackend(current_entry->backend, std::move(http));
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to create backend: " << e.what() << "\n";
    return 1;
  }

  std::cout << "[INFO] Connected to model: " << current_entry->name;
  if (!current_entry->description.empty()) {
    std::cout << " (" << current_entry->description << ")";
  }
  std::cout << "\nType /help for available commands.\n\n";

  std::vector<pu::backend::Message> history;
  std::string input;

  while (true) {
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, input)) break;
    if (input.empty()) continue;

    if (input[0] == '/') {
      if (input == "/help") {
        PrintHelp();
      } else if (input == "/exit" || input == "/quit") {
        break;
      } else if (input == "/clear") {
        history.clear();
        std::cout << "[INFO] Conversation history cleared.\n";
      } else if (input == "/models") {
        PrintModels(models, current_entry->name);
      } else if (input.rfind("/model ", 0) == 0) {
        std::string new_name = input.substr(7);
        new_name.erase(0, new_name.find_first_not_of(" \t"));
        new_name.erase(new_name.find_last_not_of(" \t") + 1);
        if (new_name.empty()) {
          std::cerr << "Error: model name required.\n";
          continue;
        }
        const pu::config::ModelEntry* new_entry = nullptr;
        for (const auto& entry : models.models) {
          if (entry.name == new_name) {
            new_entry = &entry;
            break;
          }
        }
        if (!new_entry) {
          std::cerr << "Error: model '" << new_name << "' not found.\n";
          continue;
        }
        auto new_http = std::make_unique<pu::http::CurlHttpClient>();
        std::unique_ptr<pu::backend::Backend> new_backend;
        try {
          new_backend = pu::config::CreateBackend(new_entry->backend, std::move(new_http));
        } catch (const std::exception& e) {
          std::cerr << "Error: failed to switch model: " << e.what() << "\n";
          continue;
        }
        backend = std::move(new_backend);
        current_entry = new_entry;
        history.clear();
        std::cout << "[INFO] Switched to model: " << current_entry->name;
        if (!current_entry->description.empty()) {
          std::cout << " (" << current_entry->description << ")";
        }
        std::cout << "\n[INFO] Conversation history cleared.\n";
      } else {
        std::cerr << "Unknown command: " << input << "\nType /help for available commands.\n";
      }
      continue;
    }

    history.push_back({pu::backend::Message::Role::kUser, input});
    try {
      auto renderer_cb = pu::StreamingRenderer::Create(/*show_reasoning=*/false);
      backend->Chat(history, renderer_cb);
      std::cout << std::endl;
    } catch (const std::exception& e) {
      std::cerr << "\nError: " << e.what() << "\n\n";
      if (!history.empty() && history.back().role == pu::backend::Message::Role::kUser) {
        history.pop_back();
      }
    }
  }

  std::cout << "\nGoodbye!\n";
  return 0;
}

}  // namespace pu::cli
