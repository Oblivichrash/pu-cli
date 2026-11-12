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


int RunDelegate(const std::string& agent, const std::string& prompt, Runtime& runtime,
                bool show_usage) {
  // The table is read here rather than through Initialize, because the turn does not run in
  // this runtime: the one that answers is built inside Delegate, from the same file.
  DelegationResult result;
  {
    Delegation request;
    request.agent = agent;
    request.prompt = prompt;
    try {
      runtime.Initialize();
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      return static_cast<int>(ExitCode::kUsage);
    }
    result = runtime.Delegate(request);
  }

  if (!result.error.empty()) {
    std::cerr << result.error << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }
  std::cout << result.answer << "\n";
  if (show_usage) {
    // What the turn cost, on stderr, in the shape `pu ask --show-usage` prints: the counts
    // always, a price only where the product reported one.
    std::cerr << "usage: " << result.usage.turns << " turn(s), " << result.usage.prompt_tokens
              << " prompt tokens, " << result.usage.completion_tokens << " completion tokens";
    if (result.usage.cost) {
      std::cerr << ", " << result.usage.cost->amount << " " << result.usage.cost->unit;
    }
    std::cerr << "\n";
  }
  return static_cast<int>(ExitCode::kOk);
}

int RunAgents(Runtime& runtime, bool as_json) {
  // The session decides which agent answers, and a workspace may have none yet.
  std::string active;
  if (runtime.DefaultSession()) active = runtime.DefaultSession()->Spec().agent_name;

  const std::vector<config::AgentSummary> agents =
      config::SummarizeAgents(runtime.Agents(), active);
  if (as_json) {
    std::cout << boost::json::serialize(config::AgentsAsJson(agents)) << "\n";
  } else {
    std::cout << config::FormatAgentTable(agents);
  }
  return static_cast<int>(ExitCode::kOk);
}

}  // namespace pu::cli
