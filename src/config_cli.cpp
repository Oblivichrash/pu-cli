// SPDX-License-Identifier: GPL-3.0-only
#include "pu/cli.hpp"

#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pu/agent_config.hpp"
#include "config_tools/agent_crud.hpp"
#include "config_tools/model_scanner.hpp"
#include "config_tools/provider_registry.hpp"
#include "config_tools/system_probe.hpp"

namespace {

// Forward declarations used by the subcommand table below.
int RefreshModels(const std::optional<std::string>& provider_opt, bool json_output);
int Probe(bool json_output);

struct Subcommand {
  const char* key;          // dispatch key (argv[1])
  const char* usage;        // help column, e.g. "show <name>"
  const char* description;  // help text
  bool needs_config;        // whether FindConfigPath must resolve before dispatch
  int (*handler)(const std::string& config_path,
                 const std::vector<std::string>& args,
                 bool json_output,
                 const std::optional<std::string>& provider_opt);
};

// Single source of truth for both dispatch and help text.
constexpr std::array<Subcommand, 8> kSubcommands = {{
    {"list", "list", "List all agents", true,
     [](const std::string& cfg, const std::vector<std::string>&, bool json,
        const std::optional<std::string>&) {
       return pu::config_tools::ListAgents(cfg, json);
     }},
    {"show", "show <name>", "Show agent details", true,
     [](const std::string& cfg, const std::vector<std::string>& args, bool json,
        const std::optional<std::string>&) {
       if (args.empty()) {
         std::cerr << "Usage: pu config show <name>\n";
         return 1;
       }
       return pu::config_tools::ShowAgent(cfg, args[0], json);
     }},
    {"add", "add [name]", "Add a new agent (interactive wizard)", true,
     [](const std::string& cfg, const std::vector<std::string>& args, bool,
        const std::optional<std::string>&) {
       std::string name = args.empty() ? "" : args[0];
       return pu::config_tools::AddAgent(cfg, name);
     }},
    {"remove", "remove <name>", "Remove an agent", true,
     [](const std::string& cfg, const std::vector<std::string>& args, bool,
        const std::optional<std::string>&) {
       if (args.empty()) {
         std::cerr << "Usage: pu config remove <name>\n";
         return 1;
       }
       return pu::config_tools::RemoveAgent(cfg, args[0]);
     }},
    {"rename", "rename <old> <new>", "Rename an agent", true,
     [](const std::string& cfg, const std::vector<std::string>& args, bool,
        const std::optional<std::string>&) {
       if (args.size() < 2) {
         std::cerr << "Usage: pu config rename <old> <new>\n";
         return 1;
       }
       return pu::config_tools::RenameAgent(cfg, args[0], args[1]);
     }},
    {"set-default", "set-default <name>", "Set the default agent", true,
     [](const std::string& cfg, const std::vector<std::string>& args, bool,
        const std::optional<std::string>&) {
       if (args.empty()) {
         std::cerr << "Usage: pu config set-default <name>\n";
         return 1;
       }
       return pu::config_tools::SetDefaultAgent(cfg, args[0]);
     }},
    {"refresh-models", "refresh-models", "Scan providers for available models", false,
     [](const std::string&, const std::vector<std::string>&, bool json,
        const std::optional<std::string>& provider) {
       return RefreshModels(provider, json);
     }},
    {"probe", "probe", "Show system and provider status", false,
     [](const std::string&, const std::vector<std::string>&, bool json,
        const std::optional<std::string>&) {
       return Probe(json);
     }},
}};

void PrintUsage() {
  std::cout << "Usage: pu config <subcommand> [options]\n"
            << "Subcommands:\n";
  for (const auto& cmd : kSubcommands) {
    std::cout << "  " << std::left << std::setw(21) << cmd.usage
              << cmd.description << "\n";
  }
  std::cout << "Options:\n"
            << "  --json               Output in JSON format (list, show, refresh-models, probe)\n"
            << "  --provider=<name>    Only scan one provider (see providers.json)\n"
            << "  -h, --help           Show this help message\n";
}

struct ScanResult {
  std::vector<std::string> models;
  std::string error;
};

ScanResult ScanOne(const pu::config_tools::ProviderConfig& config) {
  ScanResult result;
  try {
    result.models = pu::config_tools::scanProvider(config);
  } catch (const std::exception& e) {
    result.error = e.what();
  }
  return result;
}

int RefreshModels(const std::optional<std::string>& provider_opt, bool json_output) {
  auto providers = pu::config_tools::LoadProviders();
  if (provider_opt) {
    std::vector<pu::config_tools::ProviderConfig> filtered;
    for (const auto& p : providers) {
      if (p.name == *provider_opt) filtered.push_back(p);
    }
    if (filtered.empty()) {
      std::cerr << "unknown provider: " << *provider_opt << "\n";
      return 1;
    }
    providers = std::move(filtered);
  }

  std::vector<std::pair<std::string, ScanResult>> results;
  for (const auto& provider : providers) {
    results.emplace_back(provider.name, ScanOne(provider));
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
  for (const auto& p : pu::config_tools::LoadProviders()) {
    if (p.type == "ollama") {
      std::cout << "  " << p.name << ": is_running="
                << (status[p.name]["is_running"].get<bool>() ? "true" : "false")
                << "\n";
    } else {
      std::cout << "  " << p.name << ": has_api_key="
                << (status[p.name]["has_api_key"].get<bool>() ? "true" : "false")
                << "\n";
    }
  }
  return 0;
}

}  // unnamed namespace

int RunConfig(int argc, char* argv[]) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  std::string subcmd = argv[1];

  // `pu config --help` / `pu config -h` — help as first argument
  if (subcmd == "-h" || subcmd == "--help") {
    PrintUsage();
    return 0;
  }

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

  const Subcommand* cmd = nullptr;
  for (const auto& c : kSubcommands) {
    if (subcmd == c.key) {
      cmd = &c;
      break;
    }
  }
  if (!cmd) {
    std::cerr << "Unknown config subcommand: " << subcmd << "\n";
    PrintUsage();
    return 1;
  }

  std::string config_path;
  if (cmd->needs_config) {
    try {
      config_path = pu::config::FindConfigPath().string();
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      return 1;
    }
  }

  return cmd->handler(config_path, args, json_output, provider_opt);
}
