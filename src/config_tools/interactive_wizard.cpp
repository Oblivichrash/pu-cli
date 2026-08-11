// SPDX-License-Identifier: GPL-3.0-only
#include "interactive_wizard.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "model_scanner.hpp"
#include "prompt_templates.hpp"

namespace pu::config_tools {

namespace {

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
std::vector<std::string> FetchModels(const std::string& provider, const std::string& host,
                                     const std::string& api_key) {
  try {
    if (provider == "nim") return scanNvidiaNIM();
    if (provider == "ollama") return scanOllama();
    return scanOpenAICompatible(host, api_key);
  } catch (const std::exception& e) {
    std::cerr << "Model scan failed: " << e.what() << "\n";
    return {};
  }
}

std::string PickModel(const std::string& provider, const std::string& host,
                      const std::string& api_key) {
  auto models = FetchModels(provider, host, api_key);
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

  // Provider selection
  std::vector<std::string> providers = {"nim", "ollama", "openai"};
  std::string provider = SelectFromList("Provider", providers, "ollama");

  // Backend config and live model scan
  std::string api_key;
  if (provider == "nim") {
    entry.backend.type = config::BackendType::kOpenAI;
    entry.backend.host = "https://integrate.api.nvidia.com/v1";
    if (const char* key = std::getenv("NVIDIA_API_KEY")) api_key = key;
    if (api_key.empty()) std::cerr << "NVIDIA_API_KEY is not set; NIM scan may fail.\n";
  } else if (provider == "ollama") {
    entry.backend.type = config::BackendType::kOllama;
    entry.backend.host = "http://localhost:11434";
  } else {
    entry.backend.type = config::BackendType::kOpenAI;
    entry.backend.host = ReadLine("Host", "https://api.openai.com/v1");
    api_key = ReadLine("API key");
  }
  if (!api_key.empty()) entry.backend.api_key = api_key;

  entry.backend.model = PickModel(provider, entry.backend.host, api_key);

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
