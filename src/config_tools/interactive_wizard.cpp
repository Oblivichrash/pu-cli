// SPDX-License-Identifier: GPL-3.0-only
#include "interactive_wizard.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "model_scanner.hpp"
#include "provider_registry.hpp"

namespace pu::config_tools {

namespace {

// Hardcoded fallback templates used when no prompts.json file is found,
// unreadable, or malformed. Kept identical to the original hardcoded set so
// behavior is unchanged when no external file is present.
const std::map<std::string, std::string>& HardcodedPromptTemplates() {
  static const std::map<std::string, std::string> templates = {
      {"general",
       "You are a helpful AI assistant. Answer the user's questions accurately and concisely."},
      {"code-review",
       "You are a senior code reviewer. Analyze the provided code for correctness, "
       "security, performance, and style issues. Provide specific, actionable feedback."},
      {"documentation",
       "You are a technical documentation writer. Produce clear, well-structured "
       "documentation with examples where appropriate."},
      {"ops",
       "You are a DevOps engineer. Help with system administration, deployment, "
       "monitoring, and infrastructure tasks. Prioritize safety and best practices."},
  };
  return templates;
}

// Returns the first prompts.json found, in priority order:
//   1. ./prompts.json          (current directory)
//   2. ~/.pu/prompts.json      (user directory)
//   3. PU_DATA_DIR/prompts.json (system install directory)
// Returns an empty path if none exist.
std::filesystem::path FindPromptsFile() {
  const std::filesystem::path local = "./prompts.json";
  if (std::filesystem::exists(local)) return local;

  if (const char* home = std::getenv("HOME")) {
    const std::filesystem::path user = std::filesystem::path(home) / ".pu" / "prompts.json";
    if (std::filesystem::exists(user)) return user;
  }

#ifdef PU_DATA_DIR
  const std::filesystem::path system = std::filesystem::path(PU_DATA_DIR) / "prompts.json";
  if (std::filesystem::exists(system)) return system;
#endif

  return {};
}

// Loads templates from a prompts.json file. Returns true on success and
// populates `out`. Returns false if the file is missing, unparseable, or
// contains no templates.
bool LoadTemplatesFromFile(const std::filesystem::path& path,
                           std::map<std::string, std::string>& out) {
  std::ifstream file(path);
  if (!file.is_open()) {
    spdlog::debug("Failed to open prompts file: {}", path.string());
    return false;
  }

  nlohmann::json j;
  try {
    file >> j;
  } catch (const std::exception& e) {
    spdlog::debug("Failed to parse prompts file {}: {}", path.string(), e.what());
    return false;
  }

  if (!j.contains("templates") || !j["templates"].is_array()) {
    spdlog::debug("Prompts file {} has no 'templates' array", path.string());
    return false;
  }

  out.clear();
  for (const auto& item : j["templates"]) {
    if (!item.is_object()) continue;
    const std::string name = item.value("name", "");
    const std::string prompt = item.value("prompt", "");
    if (name.empty() || prompt.empty()) continue;
    out[name] = prompt;
  }

  if (out.empty()) {
    spdlog::debug("Prompts file {} contains no valid templates", path.string());
    return false;
  }

  return true;
}

// Returns the effective prompt templates: tries to load from the first
// available prompts.json file; falls back to the hardcoded map on any failure.
const std::map<std::string, std::string>& GetPromptTemplates() {
  static const std::map<std::string, std::string> templates = []() {
    const auto path = FindPromptsFile();
    if (path.empty()) {
      spdlog::debug("No prompts.json found; using hardcoded templates");
      return HardcodedPromptTemplates();
    }

    std::map<std::string, std::string> loaded;
    if (LoadTemplatesFromFile(path, loaded)) {
      spdlog::debug("Loaded {} prompt templates from {}", loaded.size(), path.string());
      return loaded;
    }

    spdlog::debug("Failed to load prompts from {}; using hardcoded templates", path.string());
    return HardcodedPromptTemplates();
  }();
  return templates;
}

std::string ReadLine(const std::string& prompt, const std::string& default_value = "") {
  std::cout << prompt;
  if (!default_value.empty()) std::cout << " [" << default_value << "]";
  std::cout << ": " << std::flush;
  std::string line;
  std::getline(std::cin, line);
  if (line.empty()) return default_value;
  return line;
}


std::string SelectFromList(const std::string& prompt,
                           const std::vector<std::string>& options,
                           const std::string& default_value = "") {
  std::cout << prompt << ":\n";
  for (size_t i = 0; i < options.size(); ++i) {
    std::cout << "  " << (i + 1) << ". " << options[i];
    if (options[i] == default_value) std::cout << " (default)";
    std::cout << "\n";
  }
  std::string line = ReadLine("Select (1-" + std::to_string(options.size()) + ")");
  if (line.empty() && !default_value.empty()) return default_value;
  try {
    size_t idx = std::stoul(line);
    if (idx >= 1 && idx <= options.size()) return options[idx - 1];
  } catch (...) {
  }
  std::cerr << "Invalid selection, using default.\n";
  return default_value.empty() ? options[0] : default_value;
}

std::vector<std::string> MultiSelect(const std::string& prompt,
                                     const std::vector<std::string>& options) {
  std::vector<std::string> selected;
  std::cout << prompt << " (enter comma-separated numbers, or 'all'):\n";
  for (size_t i = 0; i < options.size(); ++i) {
    std::cout << "  " << (i + 1) << ". " << options[i] << "\n";
  }
  std::string line = ReadLine("Select");
  if (line == "all") return options;
  size_t pos = 0;
  while (pos < line.size()) {
    size_t comma = line.find(',', pos);
    std::string token = line.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    try {
      size_t idx = std::stoul(token);
      if (idx >= 1 && idx <= options.size()) selected.push_back(options[idx - 1]);
    } catch (...) {
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return selected;
}

// Live scans can fail (service down, missing key); fall back to manual entry.
std::vector<std::string> FetchModels(const ProviderConfig& config) {
  try {
    return scanProvider(config);
  } catch (const std::exception& e) {
    std::cerr << "Model scan failed: " << e.what() << "\n";
    return {};
  }
}

std::string PickModel(const ProviderConfig& config) {
  auto models = FetchModels(config);
  if (models.empty()) {
    return ReadLine("Model name (no models discovered)");
  }
  return SelectFromList("Model", models, models[0]);
}

}  // unnamed namespace

config::AgentEntry RunInteractiveWizard() {
  config::AgentEntry entry;

  entry.name = ReadLine("Agent name");
  if (entry.name.empty()) {
    std::cerr << "Agent name is required.\n";
    throw std::runtime_error("Agent name is required");
  }

  entry.description = ReadLine("Description");

  // Provider list comes from the registry so custom providers defined in
  // providers.json show up without code changes.
  const auto providers = LoadProviders();
  std::vector<std::string> provider_names;
  for (const auto& p : providers) provider_names.push_back(p.name);
  std::string default_provider =
      std::find(provider_names.begin(), provider_names.end(), "ollama") !=
              provider_names.end()
          ? "ollama"
          : provider_names.front();
  std::string provider_name =
      SelectFromList("Provider", provider_names, default_provider);
  const ProviderConfig* chosen = nullptr;
  for (const auto& p : providers) {
    if (p.name == provider_name) {
      chosen = &p;
      break;
    }
  }
  if (!chosen) {
    throw std::runtime_error("Unknown provider: " + provider_name);
  }

  // Backend config and live model scan, both driven by the chosen provider.
  std::string api_key;
  entry.backend.type = chosen->type == "ollama" ? config::BackendType::kOllama
                                                : config::BackendType::kOpenAI;
  entry.backend.host = chosen->base_url;
  if (!chosen->auth.env_var.empty()) {
    if (const char* key = std::getenv(chosen->auth.env_var.c_str())) api_key = key;
    if (api_key.empty()) {
      std::cerr << chosen->auth.env_var
                << " is not set; model scan may fail.\n";
    }
  }
  if (!api_key.empty()) entry.backend.api_key = api_key;

  entry.backend.model = PickModel(*chosen);

  // Reasoning effort is only meaningful for openai_compatible providers.
  if (chosen->type == "openai_compatible") {
    std::string effort = ReadLine("Reasoning effort (default, low, medium, high)", "default");
    if (effort == "low" || effort == "medium" || effort == "high") {
      entry.backend.reasoning_effort = effort;
    } else if (effort != "default") {
      std::cerr << "Unknown reasoning effort '" << effort
                << "', using default (none).\n";
    }
    // "default" (or empty input) leaves reasoning_effort as nullopt.
  }

  // Tools multi-select
  std::vector<std::string> tool_options = {"execute_bash", "write_file", "ask_user"};
  entry.tools = MultiSelect("Select tools", tool_options);

  // Sandbox root
  entry.security.sandbox_root = ReadLine("Sandbox root", ".");

  // Forbidden patterns
  std::vector<std::string> default_forbidden = {"rm -rf", "sudo", "mkfs", "dd", "chmod 777", "cd"};
  std::string forbidden_line = ReadLine("Forbidden patterns (comma-separated, empty for defaults)");
  if (forbidden_line.empty()) {
    entry.security.forbidden_patterns = default_forbidden;
  } else {
    size_t pos = 0;
    while (pos < forbidden_line.size()) {
      size_t comma = forbidden_line.find(',', pos);
      std::string token = forbidden_line.substr(
          pos, comma == std::string::npos ? std::string::npos : comma - pos);
      if (!token.empty()) entry.security.forbidden_patterns.push_back(token);
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }

  // System prompt template selection
  const auto& templates = GetPromptTemplates();
  std::vector<std::string> template_names;
  for (const auto& [name, _] : templates) template_names.push_back(name);
  std::string tmpl = SelectFromList("System prompt template", template_names, "general");
  entry.backend.system_prompt = templates.at(tmpl);

  return entry;
}

}  // namespace pu::config_tools