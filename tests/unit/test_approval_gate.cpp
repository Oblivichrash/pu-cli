// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <string>

#include "pu/config/security_policy.hpp"
#include "pu/tools/builtin_tools.hpp"

using namespace pu;

namespace {

ToolContext GateContext(config::SecurityPolicy& policy, bool allow, std::string note,
                        bool* asked) {
  ToolContext ctx;
  ctx.security = &policy;
  static ToolContext::Approval approval;
  approval.ask = [asked, allow, note](const std::string&, std::string& out_note) {
    if (asked) *asked = true;
    out_note = note;
    return allow;
  };
  ctx.approval = &approval;
  return ctx;
}

}  // namespace

TEST_CASE("a command matching an approval pattern asks and runs when allowed",
          "[builtin_tools][approval]") {
  tools::ExecuteBashToolStandard tool(".");
  config::SecurityPolicy policy;
  policy.approval_patterns = {"curl"};
  bool asked = false;
  auto ctx = GateContext(policy, true, "", &asked);

  boost::json::value args = {{"command", "curl example.com"}};
  std::string result = tool.Execute(args, ctx);

  REQUIRE(asked);
  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == true);
}

TEST_CASE("a denied command is refused with the user's note beside it",
          "[builtin_tools][approval]") {
  tools::ExecuteBashToolStandard tool(".");
  config::SecurityPolicy policy;
  policy.approval_patterns = {"curl"};
  bool asked = false;
  auto ctx = GateContext(policy, false, "use the proxy instead", &asked);

  boost::json::value args = {{"command", "curl example.com"}};
  std::string result = tool.Execute(args, ctx);

  REQUIRE(asked);
  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("use the proxy instead") !=
          std::string::npos);
}

TEST_CASE("a command the gateway has no channel for is refused, not silently run",
          "[builtin_tools][approval]") {
  tools::ExecuteBashToolStandard tool(".");
  config::SecurityPolicy policy;
  policy.approval_patterns = {"curl"};

  ToolContext ctx;
  ctx.security = &policy;  // no approval channel

  boost::json::value args = {{"command", "curl example.com"}};
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("blocked") == true);
}

TEST_CASE("a command matching no approval pattern never asks", "[builtin_tools][approval]") {
  tools::ExecuteBashToolStandard tool(".");
  config::SecurityPolicy policy;
  policy.approval_patterns = {"curl"};
  bool asked = false;
  auto ctx = GateContext(policy, false, "", &asked);

  boost::json::value args = {{"command", "echo hi"}};
  std::string result = tool.Execute(args, ctx);

  REQUIRE_FALSE(asked);
  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == true);
}
