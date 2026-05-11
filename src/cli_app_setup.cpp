// SPDX-License-Identifier: GPL-3.0-only

#include "pu/cli_app_setup.hpp"

#include "pu/expert_config.hpp"
#include "pu/http/http_client.hpp"
#include "pu/expert.hpp"
#include "pu/backend.hpp"

#include "experts/chat/chat_expert.hpp"
#include "experts/bash/bash_expert.hpp"
#include "http/curl_http_client.hpp"
#include "executor/command_executor.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace pu::cli {

AppContext SetupAppContext(const std::string& requested_expert,
                          bool show_reasoning) {
  AppContext ctx;

  // 1. Locate and load configuration
  try {
    ctx.config_path = config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::exit(1);
  }

  try {
    ctx.config = config::LoadExpertsConfig(ctx.config_path);
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to load config: " << e.what() << "\n";
    std::exit(1);
  }

  if (ctx.config.experts.empty()) {
    std::cerr << "Error: no experts configured\n";
    std::exit(1);
  }

  // 2. Determine which expert should be active initially
  std::string active_name = requested_expert.empty()
                                ? ctx.config.default_expert
                                : requested_expert;
  bool active_found = false;

  // 3. Register all experts
  for (const auto& entry : ctx.config.experts) {
    if (entry.name == active_name) {
      active_found = true;
    }

    std::unique_ptr<pu::http::HttpClient> http =
        std::make_unique<pu::http::CurlHttpClient>();

    if (entry.type == config::ExpertType::kChat) {
      auto backend = config::CreateBackend(entry.backend, std::move(http));
      ctx.manager.RegisterExpert(
          std::make_unique<experts::ChatExpert>(entry.name,
                                                std::move(backend),
                                                entry.name));
    } else if (entry.type == config::ExpertType::kBash) {
      auto backend = config::CreateBackend(entry.backend, std::move(http));
      auto executor = std::make_unique<executor::CommandExecutor>(
          entry.sandbox_path);
      ctx.manager.RegisterExpert(
          std::make_unique<experts::BashExpert>(entry.name,
                                                std::move(backend),
                                                std::move(executor)));
    }
  }

  if (!active_found) {
    std::cerr << "Error: expert '" << active_name << "' not found\n";
    // Print available experts (simplified, can be enhanced)
    std::cerr << "Available experts:\n";
    for (const auto& e : ctx.config.experts) {
      std::cerr << "  " << e.name << "\n";
    }
    std::exit(1);
  }

  // 4. Set active expert and options
  ctx.manager.SetActiveExpert(active_name);
  if (show_reasoning) {
    ctx.manager.SetShowReasoning(true);
  }

  return ctx;
}

}  // namespace pu::cli
