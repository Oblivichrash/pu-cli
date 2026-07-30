// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/mcp_tool.hpp"
#include <spdlog/spdlog.h>

namespace pu::tools {

McpTool::McpTool(mcp::McpClient* client, const ToolDefinition& def)
    : client_(client), def_(def) {}

std::string McpTool::Execute(const nlohmann::json& args, ToolContext& /*ctx*/) {
    if (!client_) {
        return "Error: MCP client is null";
    }
    if (!client_->IsConnected()) {
        return "Error: MCP client is not connected";
    }
    return client_->CallTool(def_.name, args);
}

} // namespace pu::tools