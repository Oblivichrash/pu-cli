// SPDX-License-Identifier: GPL-3.0-only

#include "pu/cli_ask.hpp"

#include "http/curl_http_client.hpp"
#include "pu/backend.hpp"
#include "pu/expert.hpp"
#include "pu/expert_config.hpp"
#include "pu/http/http_client.hpp"
#include "pu/renderer.hpp"

#include "experts/chat/chat_expert.hpp"
#include "experts/bash/bash_expert.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace pu::cli {

namespace {

void PrintUsage() {
  std::cerr << "Usage: pu ask [--expert <name>] [--show-reasoning] <prompt>\n"
            << "Options:\n"
            << "  --expert <name>     Specify the expert to use (default: from config)\n"
            << "  --show-reasoning    Show model's internal reasoning\n"
            << "  -h, --help          Show this help message\n";
}

void PrintAvailableExperts(const pu::config::ExpertsConfig& config) {
  std::cerr << "Available experts:\n";
  for (const auto& entry : config.experts) {
    std::cerr << "  " << entry.name;
    if (!entry.description.empty()) {
      std::cerr << " - " << entry.description;
    }
    std::cerr << "\n";
  }
}

}  // namespace

int RunAskCommand(int argc, char* argv[]) {
  std::string expert_name;
  std::string prompt;
  bool show_reasoning = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    } else if (arg == "--expert") {
      if (i + 1 < argc) {
        expert_name = argv[++i];
      } else {
        std::cerr << "Error: --expert requires an argument\n";
        PrintUsage();
        return 1;
      }
    } else if (arg == "--show-reasoning") {
      show_reasoning = true;
    } else if (prompt.empty()) {
      prompt = arg;
    } else {
      std::cerr << "Error: unexpected argument '" << arg << "'\n";
      PrintUsage();
      return 1;
    }
  }

  if (prompt.empty()) {
    std::cerr << "Error: prompt is required\n";
    PrintUsage();
    return 1;
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

  std::string target_name = expert_name.empty() ? config.default_expert : expert_name;
  const pu::config::ExpertEntry* target_entry = nullptr;
  for (const auto& entry : config.experts) {
    if (entry.name == target_name) {
      target_entry = &entry;
      break;
    }
  }
  if (!target_entry) {
    std::cerr << "Error: expert '" << target_name << "' not found\n";
    PrintAvailableExperts(config);
    return 1;
  }

  auto chat_http = std::make_unique<pu::http::CurlHttpClient>();
  auto chat_backend = pu::config::CreateBackend(target_entry->backend, std::move(chat_http));

  auto router_http = std::make_unique<pu::http::CurlHttpClient>();
  auto router_backend = pu::config::CreateBackend(target_entry->backend, std::move(router_http));

  pu::backend::Backend* router_raw = router_backend.get();
  pu::expert::ExpertManager manager(std::move(router_backend));
  manager.RegisterExpert(
      std::make_unique<pu::experts::ChatExpert>(std::move(chat_backend), target_entry->name));
  manager.RegisterExpert(
      std::make_unique<pu::experts::BashExpert>(*router_raw,
                                                std::make_unique<pu::executor::CommandExecutor>(".")));

  if (!target_name.empty()) {
    manager.SetActiveExpert(target_name);
  }
  if (show_reasoning) {
    manager.SetShowReasoning(true);
  }

  try {
    manager.Dispatch(prompt);
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << "\n";
    return 1;
  }

  return 0;
}

}  // namespace pu::cli
