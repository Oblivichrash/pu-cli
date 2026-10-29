// SPDX-License-Identifier: GPL-3.0-only

//
// A tool call the policy parks for approval, met by every front end `pu` has.
//
// The question is not whether the sandbox refuses — it does, and says why — but what the
// caller is told to do about it: rework nothing, because the command was fine. That is the
// one refusal whose remedy is a configuration, and it has to reach the exit status.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <memory>
#include <string>

#include "pu/runtime/executor.hpp"
#include "pu/paging/paging.hpp"
#include "pu/tools/bash.hpp"

namespace {

using pu::ToolContext;
namespace config = pu::config;
namespace tools = pu::tools;

// The store is the session's; a test borrows one for as long as it runs.
pu::paging::Pager& Store() {
  static pu::paging::Pager pager;
  return pager;
}

// The policy names one command for approval. No approval callback is installed, which is
// what both front ends look like: `pu ask` has no channel, and the CLI's chat has none
// either.
ToolContext ContextNeedingApproval(const config::SecurityPolicy& policy) {
  ToolContext ctx;
  ctx.security = &policy;
  return ctx;
}

config::SecurityPolicy PolicyNaming(const std::string& pattern) {
  config::SecurityPolicy policy;
  policy.sandbox_root = ".";
  policy.approval_patterns = {pattern};
  return policy;
}

}  // namespace

TEST_CASE("A command the policy parks for approval is refused when nobody can answer",
          "[tools][refusal]") {
  const config::SecurityPolicy policy = PolicyNaming("my_script");
  ToolContext ctx = ContextNeedingApproval(policy);
  tools::BashTool tool(policy.sandbox_root, Store());

  const std::string raw = tool.Execute(boost::json::object{{"command", "my_script --go"}}, ctx);

  const tools::ToolResult parsed = tools::ParseToolResult(raw);
  REQUIRE(parsed.valid);
  REQUIRE(parsed.blocked);
  // The whole point: the refusal names itself as the kind a configuration fixes, not the
  // kind a command rework fixes.
  REQUIRE(parsed.refusal == tools::Refusal::kNobodyToAsk);
  REQUIRE(parsed.error.find("no approval channel") != std::string::npos);
}

TEST_CASE("The same policy leaves a command it does not name alone", "[tools][refusal]") {
  // A guard against the pattern being read as "every command needs approval": the refusal
  // above has to be about this command, not about the policy being present.
  const config::SecurityPolicy policy = PolicyNaming("my_script");
  ToolContext ctx = ContextNeedingApproval(policy);
  tools::BashTool tool(policy.sandbox_root, Store());

  const std::string raw = tool.Execute(boost::json::object{{"command", "true"}}, ctx);
  const tools::ToolResult parsed = tools::ParseToolResult(raw);

  REQUIRE(parsed.valid);
  REQUIRE_FALSE(parsed.blocked);
  REQUIRE(parsed.refusal == tools::Refusal::kNone);
}
