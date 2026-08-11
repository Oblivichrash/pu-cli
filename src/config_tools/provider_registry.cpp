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

// Kept in sync with the pre-externalization hardcoded scanners.
std::vector<ProviderConfig> BuiltinProviders() {
  return {
      {"nim", "openai_compatible", "https://integrate.api.nvidia.com/v1",
       {"NVIDIA_API_KEY"}, "/models", "data[].id"},
      {"ollama", "ollama", "http://localhost:11434", {}, "/api/tags",
       "models[].name"},
      {"openai", "openai_compatible", "https://api.openai.com/v1",
       {"OPENAI_API_KEY"}, "/models", "data[].id"},
  };
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
  // A malformed file silently falls back to defaults instead of making the
  // whole CLI unusable; the warning points the user at the offending file.
  for (const auto& path : {LocalProvidersPath(), UserProvidersPath()}) {
    if (path.empty() || !std::filesystem::exists(path)) continue;
    std::ifstream file(path);
    if (!file.is_open()) continue;
    nlohmann::json j;
    try {
      file >> j;
    } catch (const std::exception& e) {
      spdlog::warn("Failed to parse {}: {}; using built-in providers",
                   path.string(), e.what());
      return BuiltinProviders();
    }
    auto providers = ParseProvidersJson(j);
    if (!providers.empty()) return providers;
    spdlog::warn("No providers found in {}; using built-in providers",
                 path.string());
  }
  return BuiltinProviders();
}

}  // namespace pu::config_tools
