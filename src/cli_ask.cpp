// SPDX-License-Identifier: GPL-3.0-only

#include "pu/cli_ask.hpp"

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

void PrintUsage() {
  std::cerr << "Usage: pu ask [-m <model>] <prompt>\n"
            << "Options:\n"
            << "  -m, --model <model>  Specify the model to use (default: from config)\n"
            << "  -h, --help           Show this help message\n";
}

void PrintAvailableModels(const pu::config::ModelsFile& models) {
  std::cerr << "Available models:\n";
  for (const auto& entry : models.models) {
    std::cerr << "  " << entry.name;
    if (!entry.description.empty()) {
      std::cerr << " - " << entry.description;
    }
    std::cerr << "\n";
  }
}

}  // namespace

int RunAskCommand(int argc, char* argv[]) {
  std::string model_name;
  std::string prompt;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    } else if (arg == "-m" || arg == "--model") {
      if (i + 1 < argc) {
        model_name = argv[++i];
      } else {
        std::cerr << "Error: --model requires an argument\n";
        PrintUsage();
        return 1;
      }
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

  const pu::config::ModelEntry* target_entry = nullptr;
  if (model_name.empty()) {
    for (const auto& entry : models.models) {
      if (entry.name == models.default_model) {
        target_entry = &entry;
        break;
      }
    }
    if (!target_entry) {
      std::cerr << "Error: default model '" << models.default_model << "' not found\n";
      PrintAvailableModels(models);
      return 1;
    }
  } else {
    for (const auto& entry : models.models) {
      if (entry.name == model_name) {
        target_entry = &entry;
        break;
      }
    }
    if (!target_entry) {
      std::cerr << "Error: model '" << model_name << "' not found\n";
      PrintAvailableModels(models);
      return 1;
    }
  }

  auto http = std::make_unique<pu::http::CurlHttpClient>();
  std::unique_ptr<pu::backend::Backend> backend;
  try {
    backend = pu::config::CreateBackend(target_entry->backend, std::move(http));
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to create backend: " << e.what() << "\n";
    return 1;
  }

  std::vector<pu::backend::Message> history;
  history.push_back({pu::backend::Message::Role::kUser, prompt});

  try {
    auto renderer_cb = pu::StreamingRenderer::Create(/*show_reasoning=*/false);
    backend->Chat(history, renderer_cb);
  } catch (const std::exception& e) {
    std::cerr << "\nError: " << e.what() << "\n";
    return 1;
  }

  return 0;
}

}  // namespace pu::cli
