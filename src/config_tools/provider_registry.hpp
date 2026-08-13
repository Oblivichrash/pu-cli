// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

namespace pu::config_tools {

// Credentials are never stored in providers.json; only the env var name is.
struct ProviderAuth {
  std::string env_var;
};

struct ProviderConfig {
  std::string name;
  std::string type;             // "ollama" or "openai_compatible"
  std::string base_url;
  ProviderAuth auth;
  std::string model_endpoint;
  std::string model_list_key;   // e.g. "data[].id" or "models[].name"
};

// Reads ./providers.json, then ~/.pu/providers.json. On first run (neither
// file exists) a default providers.json with nim/ollama/openai is written to
// ~/.pu/ and loaded, so a fresh checkout works offline and users can freely
// edit or extend the file afterwards.
std::vector<ProviderConfig> LoadProviders();

}  // namespace pu::config_tools
