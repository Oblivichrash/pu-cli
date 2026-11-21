// SPDX-License-Identifier: GPL-3.0-only
#include "pu/app/agent_summary.hpp"

#include <sstream>

#include "pu/config/agent_manager.hpp"
#include "pu/model/backend.hpp"
#include "pu/model/gateway.hpp"

namespace pu::app {

AgentSummary SummarizeAgent(const AgentManager& agents) {
  AgentSummary summary;
  const std::vector<std::string> names = agents.AgentNames();
  if (names.empty()) return summary;
  summary.name = names.front();
  if (const config::AgentEntry* entry = agents.AgentConfig(summary.name); entry != nullptr) {
    summary.description = entry->description;
    summary.gateway = model::GatewayName(entry->backend.gateway);
    summary.model = entry->backend.model;
  }
  return summary;
}

boost::json::value AgentAsJson(const AgentSummary& agent) {
  if (agent.name.empty()) return nullptr;
  return boost::json::value{{"name", agent.name},
                            {"description", agent.description},
                            {"gateway", agent.gateway},
                            {"model", agent.model}};
}

std::string FormatAgent(const AgentSummary& agent) {
  if (agent.name.empty()) return "No agent is configured.\n";

  std::ostringstream out;
  out << agent.name << "\n";
  if (!agent.description.empty()) out << "  " << agent.description << "\n";
  if (!agent.gateway.empty()) {
    out << "  " << agent.gateway;
    if (!agent.model.empty()) out << " / " << agent.model;
    out << "\n";
  }
  return out.str();
}

}  // namespace pu::app
