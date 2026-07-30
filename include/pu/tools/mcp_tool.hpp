// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/tools/tool.hpp"
#include "pu/mcp/mcp_client.hpp"
#include <memory>

namespace pu::tools {

class McpTool : public Tool {
public:
    McpTool(mcp::McpClient* client, const ToolDefinition& def);
    ~McpTool() override = default;

    std::string Name() const override { return "mcp." + def_.name; }
    std::string Description() const override { return def_.description; }
    std::string ParametersSchema() const override { return def_.parameters_schema; }
    std::string Execute(const nlohmann::json& args, ToolContext& ctx) override;

private:
    mcp::McpClient* client_;
    ToolDefinition def_;
};

} // namespace pu::tools