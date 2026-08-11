// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config_cli.hpp"

#include <iostream>
#include <string>

#include "pu/agent_config.hpp"
#include "config_tools/agent_crud.hpp"

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
            << "Options:\n"
            << "  --json               Output in JSON format (list, show)\n"
            << "  -h, --help           Show this help message\n";
}

}  // unnamed namespace

int RunConfig(int argc, char* argv[]) {
  if (argc < 2) {
    PrintUsage();
    return 1;
  }

  std::string subcmd = argv[1];
  bool json_output = false;
  std::vector<std::string> args;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--json") {
      json_output = true;
    } else if (arg == "-h" || arg == "--help") {
      PrintUsage();
      return 0;
    } else {
      args.push_back(arg);
    }
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