// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/tools/toolbox.hpp"
#include "pu/mcp/mcp_client.hpp"
#include <memory>
#include <string>

namespace pu::tools {

class McpTool : public Tool {
public:
    /// @param client       Raw pointer to the owning McpClient (must outlive this tool).
    /// @param def          The tool definition as returned by the MCP server.
    /// @param server_name  The name of the MCP server (used for namespacing).
    McpTool(mcp::McpClient* client, const ToolDefinition& def,
            std::string server_name);
    ~McpTool() override = default;

    std::string Name() const override {
        return "mcp." + server_name_ + "." + original_tool_name_;
    }
    std::string Description() const override { return def_.description; }
    std::string ParametersSchema() const override { return def_.parameters_schema; }
    std::string Execute(const nlohmann::json& args, ToolContext& ctx) override;

private:
    mcp::McpClient* client_;
    ToolDefinition def_;
    std::string server_name_;
    std::string original_tool_name_;
};

} // namespace pu::tools