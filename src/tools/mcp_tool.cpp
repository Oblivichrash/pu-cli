// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/mcp_tool.hpp"
#include <spdlog/spdlog.h>

namespace pu::tools {

McpTool::McpTool(mcp::McpClient* client, const ToolDefinition& def,
                 std::string server_name)
    : client_(client),
      def_(def),
      server_name_(std::move(server_name)),
      original_tool_name_(def.name) {}

std::string McpTool::Execute(const nlohmann::json& args, ToolContext& /*ctx*/) {
    if (!client_) {
        return "Error: MCP client is null";
    }
    if (!client_->IsConnected()) {
        return "Error: MCP client is not connected";
    }
    return client_->CallTool(original_tool_name_, args);
}

} // namespace pu::tools