// SPDX-License-Identifier: GPL-3.0-only

//
// `pu agents`: who is configured, which one answers, and what each one is for.
//
// The chat has `/agents` and the Web page has a selector; neither is available to a script,
// and the only non-interactive way to learn a name was to ask for one that does not exist and
// read the error. This is the third surface, and it prints what the other two already show.
//

#include "pu/app/cli.hpp"

#include <boost/json.hpp>

#include <iostream>
#include <string>

#include "pu/config/agent_summary.hpp"
#include "pu/runtime/runtime.hpp"

namespace pu::cli {

int RunAgents(Runtime& runtime, bool as_json) {
  // The session decides which agent answers, and a workspace may have none yet.
  std::string active;
  if (runtime.DefaultSession()) active = runtime.DefaultSession()->Spec().agent_name;

  const std::vector<config::AgentSummary> agents = config::SummarizeAgents(runtime, active);
  if (as_json) {
    std::cout << boost::json::serialize(config::AgentsAsJson(agents)) << "\n";
  } else {
    std::cout << config::FormatAgentTable(agents);
  }
  return kOk;
}

}  // namespace pu::cli
