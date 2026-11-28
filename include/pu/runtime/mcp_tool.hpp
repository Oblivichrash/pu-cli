// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// A tool a server published, wearing the shape a turn calls.
//
// This is the one place a tool is made of an MCP call. It lives here, above both, because it is
// the join: `tools` states what a turn may call and `mcp` states how a server is reached, and
// neither has a reason to know the other exists. Written as a member of either, one of them
// would link the other for no reason but this file.
//

#include <memory>
#include <string>
#include <utility>

#include <boost/json.hpp>

#include "pu/mcp/client.hpp"
#include "pu/agent_tools/tool.hpp"

namespace pu::runtime {

class McpTool : public Tool {
 public:
  McpTool(std::shared_ptr<mcp::McpClient> client, const ToolDefinition& def,
          std::string server_name)
      : client_(std::move(client)),
        def_(def),
        server_name_(std::move(server_name)),
        original_tool_name_(def_.name) {}

  std::string Name() const override { return "mcp." + server_name_ + "." + original_tool_name_; }
  std::string Description() const override { return def_.description; }
  boost::json::value ParametersSchema() const override { return def_.parameters; }

  std::string Execute(const boost::json::value& args, ToolContext& /*ctx*/) override {
    if (!client_->IsConnected()) {
      return agent_tools::MakeToolResultJson(false, "", "MCP client is not connected", -1);
    }
    return WrapResult(client_->CallTool(original_tool_name_, args));
  }

 private:
  static std::string WrapResult(const std::string& raw) {
    const bool is_error = raw.rfind("Error:", 0) == 0 || raw.rfind("MCP error:", 0) == 0 ||
                          raw.rfind("MCP call error:", 0) == 0;
    return agent_tools::MakeToolResultJson(!is_error, raw, is_error ? raw : std::string{},
                                     is_error ? 1 : 0);
  }

  std::shared_ptr<mcp::McpClient> client_;
  ToolDefinition def_;
  std::string server_name_;
  std::string original_tool_name_;
};

}  // namespace pu::runtime
