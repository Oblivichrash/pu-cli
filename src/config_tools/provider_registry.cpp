// SPDX-License-Identifier: GPL-3.0-only
#include "provider_registry.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace pu::config_tools {

namespace {

// Local file wins over the user-wide file so a repo can pin its provider set.
std::filesystem::path LocalProvidersPath() {
  return std::filesystem::path("./providers.json");
}

std::filesystem::path UserProvidersPath() {
  if (const char* home = std::getenv("HOME")) {
    return std::filesystem::path(home) / ".pu" / "providers.json";
  }
  return {};
}

// First-run default: mirrors the providers that used to be hardcoded, so a
// fresh install behaves exactly as before while remaining fully file-driven.
void WriteDefaultProviders(const std::filesystem::path& path) {
  nlohmann::json j = {
      {"providers", nlohmann::json::array({
          {{"name", "nim"},
           {"type", "openai_compatible"},
           {"base_url", "https://integrate.api.nvidia.com/v1"},
           {"auth", {{"env_var", "NVIDIA_API_KEY"}}},
           {"model_endpoint", "/models"},
           {"model_list_key", "data[].id"}},
          {{"name", "ollama"},
           {"type", "ollama"},
           {"base_url", "http://localhost:11434"},
           {"model_endpoint", "/api/tags"},
           {"model_list_key", "models[].name"}},
          {{"name", "openai"},
           {"type", "openai_compatible"},
           {"base_url", "https://api.openai.com/v1"},
           {"auth", {{"env_var", "OPENAI_API_KEY"}}},
           {"model_endpoint", "/models"},
           {"model_list_key", "data[].id"}},
      })}};

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path);
  if (!file.is_open()) {
    spdlog::warn("Failed to write default providers file: {}", path.string());
    return;
  }
  file << j.dump(2) << "\n";
  spdlog::info("Created default providers file: {}", path.string());
}

// Accepts either a bare array or {"providers": [...]}; the object shape allows
// future top-level metadata without a breaking change.
std::vector<ProviderConfig> ParseProvidersJson(const nlohmann::json& j) {
  std::vector<ProviderConfig> providers;
  if (j.is_array()) {
    providers.reserve(j.size());
    for (const auto& item : j) {
      ProviderConfig cfg;
      cfg.name = item.value("name", "");
      if (cfg.name.empty()) continue;  // unnamed providers cannot be selected
      cfg.type = item.value("type", "openai_compatible");
      cfg.base_url = item.value("base_url", "");
      if (item.contains("auth") && item["auth"].is_object()) {
        cfg.auth.env_var = item["auth"].value("env_var", "");
      }
      cfg.model_endpoint = item.value("model_endpoint", "/models");
      cfg.model_list_key = item.value("model_list_key", "data[].id");
      providers.push_back(std::move(cfg));
    }
  } else if (j.contains("providers") && j["providers"].is_array()) {
    return ParseProvidersJson(j["providers"]);
  }
  return providers;
}

}  // namespace

std::vector<ProviderConfig> LoadProviders() {
  const auto local = LocalProvidersPath();
  const auto user = UserProvidersPath();

  // First install: no providers.json anywhere yet. Write the default set to
  // the user directory (or the local file when HOME is unavailable) and load
  // it, so provider config is file-driven from the very first run. Users can
  // freely edit or extend the file afterwards.
  if ((local.empty() || !std::filesystem::exists(local)) &&
      (user.empty() || !std::filesystem::exists(user))) {
    auto default_path = user.empty() ? local : user;
    if (!default_path.empty()) WriteDefaultProviders(default_path);
  }

  for (const auto& path : {local, user}) {
    if (path.empty() || !std::filesystem::exists(path)) continue;
    std::ifstream file(path);
    if (!file.is_open()) continue;
    nlohmann::json j;
    try {
      file >> j;
    } catch (const std::exception& e) {
      spdlog::warn("Failed to parse {}: {}", path.string(), e.what());
      return {};
    }
    auto providers = ParseProvidersJson(j);
    if (!providers.empty()) return providers;
    spdlog::warn("No providers found in {}", path.string());
  }
  return {};
}

}  // namespace pu::config_tools
