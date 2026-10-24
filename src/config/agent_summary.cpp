// SPDX-License-Identifier: GPL-3.0-only
#include "pu/config/agent_summary.hpp"

#include <algorithm>
#include <sstream>

#include "pu/config/backend.hpp"
#include "pu/config/gateway.hpp"
#include "pu/runtime/runtime.hpp"

namespace pu::config {

std::vector<AgentSummary> SummarizeAgents(const Runtime& runtime, const std::string& active) {
  std::vector<AgentSummary> agents;
  const std::vector<std::string> names = runtime.Agents().AgentNames();
  agents.reserve(names.size());
  for (const std::string& name : names) {
    const AgentEntry* entry = runtime.Agents().AgentConfig(name);
    AgentSummary summary;
    summary.name = name;
    if (entry != nullptr) {
      summary.description = entry->description;
      summary.gateway = GatewayName(entry->backend.gateway);
      summary.model = entry->backend.model;
    }
    summary.active = (name == active);
    agents.push_back(std::move(summary));
  }
  return agents;
}

boost::json::value AgentsAsJson(const std::vector<AgentSummary>& agents) {
  boost::json::array out;
  out.reserve(agents.size());
  for (const AgentSummary& agent : agents) {
    out.push_back(boost::json::value{{"name", agent.name},
                                     {"description", agent.description},
                                     {"gateway", agent.gateway},
                                     {"model", agent.model},
                                     {"active", agent.active}});
  }
  return out;
}

std::string FormatAgentTable(const std::vector<AgentSummary>& agents) {
  if (agents.empty()) return "No agents are configured.\n";

  // The name column is as wide as the longest name, so the marker lines up when one agent's
  // name is twice another's.
  std::size_t width = 0;
  for (const AgentSummary& agent : agents) width = std::max(width, agent.name.size());

  std::ostringstream out;
  out << agents.size() << " agent(s) configured:\n";
  for (const AgentSummary& agent : agents) {
    out << "  " << agent.name;
    if (agent.active) out << " (active)";
    out << "\n";
    if (!agent.description.empty()) {
      out << "  " << std::string(width, ' ') << "   " << agent.description << "\n";
    }
    if (!agent.gateway.empty()) {
      out << "  " << std::string(width, ' ') << "   " << agent.gateway;
      if (!agent.model.empty()) out << " / " << agent.model;
      out << "\n";
    }
  }
  return out.str();
}

}  // namespace pu::config
