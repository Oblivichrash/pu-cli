// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>

#include <boost/json.hpp>

#include "pu/mcp/client.hpp"
#include "pu/tools/toolbox.hpp"

namespace pu::tools {

class McpTool : public Tool {
 public:
  McpTool(std::shared_ptr<mcp::McpClient> client, const ToolDefinition& def, std::string server_name)
      : client_(std::move(client)),
        def_(def),
        server_name_(std::move(server_name)),
        original_tool_name_(def_.name) {}

  std::string Name() const override { return "mcp." + server_name_ + "." + original_tool_name_; }
  std::string Description() const override { return def_.description; }
  boost::json::value ParametersSchema() const override { return def_.parameters; }

  std::string Execute(const boost::json::value& args, ToolContext& /*ctx*/) override {
    if (!client_->IsConnected()) {
      return MakeToolResultJson(false, "", "", "MCP client is not connected", -1);
    }
    return WrapResult(client_->CallTool(original_tool_name_, args));
  }

 private:
  static std::string WrapResult(const std::string& raw) {
    const bool is_error = raw.rfind("Error:", 0) == 0 || raw.rfind("MCP error:", 0) == 0 ||
                          raw.rfind("MCP call error:", 0) == 0;
    return MakeToolResultJson(!is_error, raw, {}, is_error ? raw : std::string{}, is_error ? 1 : 0);
  }

  std::shared_ptr<mcp::McpClient> client_;
  ToolDefinition def_;
  std::string server_name_;
  std::string original_tool_name_;
};

}  // namespace pu::tools
