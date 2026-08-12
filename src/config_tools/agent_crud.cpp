// SPDX-License-Identifier: GPL-3.0-only
#include "agent_crud.hpp"

#include <algorithm>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "pu/error.hpp"
#include "interactive_wizard.hpp"

namespace pu::config_tools {

namespace {

using json = nlohmann::json;

json AgentToJson(const config::AgentEntry& entry) {
  json j;
  j["name"] = entry.name;
  j["description"] = entry.description;
  j["tools"] = entry.tools;

  json security;
  security["sandbox_root"] = entry.security.sandbox_root;
  security["allowed_paths"] = entry.security.allowed_paths;
  security["max_command_length"] = entry.security.max_command_length;
  security["forbidden_patterns"] = entry.security.forbidden_patterns;
  j["security"] = security;

  // Reuses config::to_json so max_tokens/parameters_as_string/system_prompt are
  // never dropped when listing or showing agents.
  j["backend"] = entry.backend;

  return j;
}

}  // unnamed namespace

int ListAgents(const std::string& config_path, bool json_output) {
  auto cfg = config::LoadAgentsConfig(config_path);

  if (json_output) {
    json j;
    j["default_agent"] = cfg.default_agent;
    json agents = json::array();
    for (const auto& entry : cfg.agents) agents.push_back(AgentToJson(entry));
    j["agents"] = agents;
    std::cout << j.dump(2) << "\n";
  } else {
    std::cout << "Default agent: " << cfg.default_agent << "\n";
    for (const auto& entry : cfg.agents) {
      std::cout << "  " << entry.name;
      if (!entry.description.empty()) std::cout << " - " << entry.description;
      if (entry.name == cfg.default_agent) std::cout << " [default]";
      std::cout << "\n";
    }
  }
  return 0;
}

int ShowAgent(const std::string& config_path, const std::string& name, bool json_output) {
  auto cfg = config::LoadAgentsConfig(config_path);

  auto it = std::find_if(cfg.agents.begin(), cfg.agents.end(),
                         [&](const config::AgentEntry& e) { return e.name == name; });
  if (it == cfg.agents.end()) {
    std::cerr << "Agent not found: " << name << "\n";
    return 1;
  }

  if (json_output) {
    std::cout << AgentToJson(*it).dump(2) << "\n";
  } else {
    std::cout << "Name: " << it->name << "\n";
    std::cout << "Description: " << it->description << "\n";
    std::cout << "Backend type: "
              << (it->backend.type == config::BackendType::kOpenAI ? "openai" : "ollama") << "\n";
    std::cout << "Host: " << it->backend.host << "\n";
    std::cout << "Model: " << it->backend.model << "\n";
    std::cout << "Tools: ";
    for (size_t i = 0; i < it->tools.size(); ++i) {
      if (i > 0) std::cout << ", ";
      std::cout << it->tools[i];
    }
    std::cout << "\n";
    std::cout << "Sandbox root: " << it->security.sandbox_root << "\n";
    std::cout << "Forbidden patterns: ";
    for (size_t i = 0; i < it->security.forbidden_patterns.size(); ++i) {
      if (i > 0) std::cout << ", ";
      std::cout << it->security.forbidden_patterns[i];
    }
    std::cout << "\n";
  }
  return 0;
}

int AddAgent(const std::string& config_path, const std::string& name) {
  auto cfg = config::LoadAgentsConfig(config_path);

  auto it = std::find_if(cfg.agents.begin(), cfg.agents.end(),
                         [&](const config::AgentEntry& e) { return e.name == name; });
  if (it != cfg.agents.end()) {
    std::cerr << "Agent already exists: " << name << "\n";
    return 1;
  }

  config::AgentEntry entry = RunInteractiveWizard();
  if (!name.empty()) entry.name = name;

  cfg.agents.push_back(entry);
  if (cfg.default_agent.empty()) cfg.default_agent = entry.name;

  config::SaveAgentsConfig(config_path, cfg);

  // Validate round-trip
  auto reloaded = config::LoadAgentsConfig(config_path);
  auto found = std::find_if(reloaded.agents.begin(), reloaded.agents.end(),
                            [&](const config::AgentEntry& e) { return e.name == entry.name; });
  if (found == reloaded.agents.end()) {
    std::cerr << "Failed to validate saved agent config\n";
    return 1;
  }

  std::cout << "Agent added: " << entry.name << "\n";
  return 0;
}

int RemoveAgent(const std::string& config_path, const std::string& name) {
  auto cfg = config::LoadAgentsConfig(config_path);

  auto it = std::find_if(cfg.agents.begin(), cfg.agents.end(),
                         [&](const config::AgentEntry& e) { return e.name == name; });
  if (it == cfg.agents.end()) {
    std::cerr << "Agent not found: " << name << "\n";
    return 1;
  }

  cfg.agents.erase(it);

  if (cfg.default_agent == name) {
    cfg.default_agent = cfg.agents.empty() ? "" : cfg.agents[0].name;
  }

  config::SaveAgentsConfig(config_path, cfg);
  std::cout << "Agent removed: " << name << "\n";
  return 0;
}

int RenameAgent(const std::string& config_path, const std::string& old_name,
                const std::string& new_name) {
  auto cfg = config::LoadAgentsConfig(config_path);

  auto it = std::find_if(cfg.agents.begin(), cfg.agents.end(),
                         [&](const config::AgentEntry& e) { return e.name == old_name; });
  if (it == cfg.agents.end()) {
    std::cerr << "Agent not found: " << old_name << "\n";
    return 1;
  }

  auto dup = std::find_if(cfg.agents.begin(), cfg.agents.end(),
                          [&](const config::AgentEntry& e) { return e.name == new_name; });
  if (dup != cfg.agents.end()) {
    std::cerr << "Agent already exists: " << new_name << "\n";
    return 1;
  }

  it->name = new_name;
  if (cfg.default_agent == old_name) cfg.default_agent = new_name;

  config::SaveAgentsConfig(config_path, cfg);
  std::cout << "Agent renamed: " << old_name << " -> " << new_name << "\n";
  return 0;
}

int SetDefaultAgent(const std::string& config_path, const std::string& name) {
  auto cfg = config::LoadAgentsConfig(config_path);

  auto it = std::find_if(cfg.agents.begin(), cfg.agents.end(),
                         [&](const config::AgentEntry& e) { return e.name == name; });
  if (it == cfg.agents.end()) {
    std::cerr << "Agent not found: " << name << "\n";
    return 1;
  }

  cfg.default_agent = name;
  config::SaveAgentsConfig(config_path, cfg);
  std::cout << "Default agent set: " << name << "\n";
  return 0;
}

}  // namespace pu::config_tools
