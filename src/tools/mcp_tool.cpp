// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/mcp_tool.hpp"

#include <spdlog/spdlog.h>

namespace pu::tools {

namespace {

std::string WrapMcpResult(const std::string& raw) {
  bool is_error = raw.rfind("Error:", 0) == 0 ||
                  raw.rfind("MCP error:", 0) == 0 ||
                  raw.rfind("MCP call error:", 0) == 0;
  return tools::MakeToolResultJson(!is_error, raw, std::string{},
                                   is_error ? raw : std::string{}, is_error ? 1 : 0);
}

}  // namespace

McpTool::McpTool(mcp::McpClient* client, const ToolDefinition& def,
                 std::string server_name)
    : client_(client),
      def_(def),
      server_name_(std::move(server_name)),
      original_tool_name_(def.name) {}

std::string McpTool::Execute(const nlohmann::json& args, ToolContext& /*ctx*/) {
    if (!client_) {
        return tools::MakeToolResultJson(false, "", "", "MCP client is null", -1);
    }
    if (!client_->IsConnected()) {
        return tools::MakeToolResultJson(false, "", "", "MCP client is not connected", -1);
    }
    return WrapMcpResult(client_->CallTool(original_tool_name_, args));
}

} // namespace pu::tools
