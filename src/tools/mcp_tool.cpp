// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/mcp_tool.hpp"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace pu::tools {

namespace {

std::string WrapMcpResult(const std::string& raw) {
  bool is_error = raw.rfind("Error:", 0) == 0 ||
                  raw.rfind("MCP error:", 0) == 0 ||
                  raw.rfind("MCP call error:", 0) == 0;

  nlohmann::json j;
  j["success"] = !is_error;
  j["stdout"] = raw;
  j["stderr"] = std::string{};
  j["error"] = is_error ? raw : std::string{};
  j["exit_code"] = is_error ? 1 : 0;
  return j.dump();
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
        nlohmann::json j;
        j["success"] = false;
        j["stdout"] = "";
        j["stderr"] = "";
        j["error"] = "MCP client is null";
        j["exit_code"] = -1;
        return j.dump();
    }
    if (!client_->IsConnected()) {
        nlohmann::json j;
        j["success"] = false;
        j["stdout"] = "";
        j["stderr"] = "";
        j["error"] = "MCP client is not connected";
        j["exit_code"] = -1;
        return j.dump();
    }
    return WrapMcpResult(client_->CallTool(original_tool_name_, args));
}

} // namespace pu::tools
