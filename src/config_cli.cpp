// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config_cli.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pu/agent_config.hpp"
#include "config_tools/agent_crud.hpp"
#include "config_tools/model_scanner.hpp"
#include "config_tools/system_probe.hpp"

namespace {

void PrintUsage() {
  std::cout << "Usage: pu config <subcommand> [options]\n"
            << "Subcommands:\n"
            << "  list                 List all agents\n"
            << "  show <name>          Show agent details\n"
            << "  add [name]           Add a new agent (interactive wizard)\n"
            << "  remove <name>        Remove an agent\n"
            << "  rename <old> <new>   Rename an agent\n"
            << "  set-default <name>   Set the default agent\n"
            << "  refresh-models       Scan providers for available models\n"
            << "  probe                Show system and provider status\n"
            << "Options:\n"
            << "  --json               Output in JSON format (list, show, refresh-models, probe)\n"
            << "  --provider=<name>    Only scan one provider: nim, ollama, or openai\n"
            << "  -h, --help           Show this help message\n";
}

struct ScanResult {
  std::vector<std::string> models;
  std::string error;
};

ScanResult ScanOne(const std::string& provider) {
  ScanResult result;
  try {
    if (provider == "nim") {
      result.models = pu::config_tools::scanNvidiaNIM();
    } else if (provider == "ollama") {
      result.models = pu::config_tools::scanOllama();
    } else if (provider == "openai") {
      const char* key = std::getenv("OPENAI_API_KEY");
      result.models = pu::config_tools::scanOpenAICompatible(
          "https://api.openai.com/v1", key ? key : "");
    } else {
      result.error = "unknown provider: " + provider;
    }
  } catch (const std::exception& e) {
    result.error = e.what();
  }
  return result;
}

int RefreshModels(const std::optional<std::string>& provider_opt, bool json_output) {
  std::vector<std::string> providers = {"nim", "ollama", "openai"};
  if (provider_opt) providers = {*provider_opt};

  std::vector<std::pair<std::string, ScanResult>> results;
  for (const auto& provider : providers) {
    results.emplace_back(provider, ScanOne(provider));
  }

  if (json_output) {
    nlohmann::json j;
    j["providers"] = nlohmann::json::object();
    for (const auto& [provider, r] : results) {
      nlohmann::json entry;
      entry["models"] = r.models;
      if (r.error.empty()) {
        entry["error"] = nullptr;
      } else {
        entry["error"] = r.error;
      }
      j["providers"][provider] = entry;
    }
    std::cout << j.dump(2) << "\n";
    return 0;
  }

  std::cout << "Provider   Status  Models\n";
  for (const auto& [provider, r] : results) {
    if (r.error.empty()) {
      std::cout << provider << "   ok      " << r.models.size() << "\n";
    } else {
      std::cout << provider << "   error   " << r.error << "\n";
    }
  }
  for (const auto& [provider, r] : results) {
    if (r.error.empty() && !r.models.empty()) {
      std::cout << "\n" << provider << ":\n";
      for (const auto& m : r.models) std::cout << "  " << m << "\n";
    }
  }
  return 0;
}

int Probe(bool json_output) {
  auto j = pu::config_tools::ProbeSystem();
  if (json_output) {
    std::cout << j.dump(2) << "\n";
    return 0;
  }
  std::cout << "os_name:         " << j["os_name"].get<std::string>() << "\n";
  std::cout << "kernel_version:  " << j["kernel_version"].get<std::string>() << "\n";
  std::cout << "arch:            " << j["arch"].get<std::string>() << "\n";
  std::cout << "working_dir:     " << j["working_dir"].get<std::string>() << "\n";
  std::cout << "provider_status:\n";
  const auto& status = j["provider_status"];
  std::cout << "  nim:    has_api_key="
            << (status["nim"]["has_api_key"].get<bool>() ? "true" : "false") << "\n";
  std::cout << "  ollama: is_running="
            << (status["ollama"]["is_running"].get<bool>() ? "true" : "false") << "\n";
  std::cout << "  openai: has_api_key="
            << (status["openai"]["has_api_key"].get<bool>() ? "true" : "false") << "\n";
  return 0;
}

}  // unnamed namespace

int RunConfig(int argc, char* argv[]) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  std::string subcmd = argv[1];
  bool json_output = false;
  std::optional<std::string> provider_opt;
  std::vector<std::string> args;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--json") {
      json_output = true;
    } else if (arg.rfind("--provider=", 0) == 0) {
      provider_opt = arg.substr(std::string("--provider=").size());
    } else if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    } else {
      args.push_back(arg);
    }
  }

  if (subcmd == "refresh-models") {
    return RefreshModels(provider_opt, json_output);
  }
  if (subcmd == "probe") {
    return Probe(json_output);
  }

  std::string config_path;
  try {
    config_path = pu::config::FindConfigPath();
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }

  using namespace pu::config_tools;

  if (subcmd == "list") {
    return ListAgents(config_path, json_output);
  }
  if (subcmd == "show") {
    if (args.empty()) {
      std::cerr << "Usage: pu config show <name>\n";
      return 1;
    }
    return ShowAgent(config_path, args[0], json_output);
  }
  if (subcmd == "add") {
    std::string name = args.empty() ? "" : args[0];
    return AddAgent(config_path, name);
  }
  if (subcmd == "remove") {
    if (args.empty()) {
      std::cerr << "Usage: pu config remove <name>\n";
      return 1;
    }
    return RemoveAgent(config_path, args[0]);
  }
  if (subcmd == "rename") {
    if (args.size() < 2) {
      std::cerr << "Usage: pu config rename <old> <new>\n";
      return 1;
    }
    return RenameAgent(config_path, args[0], args[1]);
  }
  if (subcmd == "set-default") {
    if (args.empty()) {
      std::cerr << "Usage: pu config set-default <name>\n";
      return 1;
    }
    return SetDefaultAgent(config_path, args[0]);
  }

  std::cerr << "Unknown config subcommand: " << subcmd << "\n";
  PrintUsage();
  return 1;
}
