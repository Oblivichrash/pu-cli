// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/mcp_tool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using namespace pu;
using namespace pu::tools;

namespace {

// A lightweight stub that mimics McpClient without requiring a real MCP
// server or transport.  We override only the two virtual methods that
// McpTool calls: CallTool and IsConnected.
class StubMcpClient : public pu::mcp::McpClient {
 public:
  StubMcpClient() : McpClient(pu::mcp::McpServerConfig{}) {}

  std::string CallTool(const std::string& /*name*/,
                       const nlohmann::json& /*arguments*/) override {
    return canned_response_;
  }

  bool IsConnected() const override { return connected_; }

  void SetResponse(const std::string& r) { canned_response_ = r; }
  void SetConnected(bool c) { connected_ = c; }

 private:
  std::string canned_response_;
  bool connected_ = true;
};

}  // namespace

TEST_CASE("McpTool wraps successful raw output into JSON schema",
          "[mcp_tool]") {
  StubMcpClient client;
  client.SetResponse("Hello from MCP server");

  ToolDefinition def;
  def.name = "read_file";
  def.description = "Reads a file";
  def.parameters_schema = R"({"type":"object"})";

  McpTool tool(&client, def, "files");

  nlohmann::json args;
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j.value("success") == true);
  REQUIRE(j.value("stdout") == "Hello from MCP server");
  REQUIRE(j.value("stderr") == "");
  REQUIRE(j.value("error") == "");
  REQUIRE(j.value("exit_code") == 0);
}

TEST_CASE("McpTool wraps Error:-prefixed output as failure JSON",
          "[mcp_tool]") {
  StubMcpClient client;
  client.SetResponse("Error: something went wrong");

  ToolDefinition def;
  def.name = "bad_tool";
  def.description = "A tool that fails";
  def.parameters_schema = "{}";

  McpTool tool(&client, def, "mcp");

  nlohmann::json args;
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j.value("success") == false);
  REQUIRE(j.value("stdout") == "Error: something went wrong");
  REQUIRE(j.value("error") == "Error: something went wrong");
  REQUIRE(j.value("exit_code") == 1);
}

TEST_CASE("McpTool wraps MCP error: output as error", "[mcp_tool]") {
  StubMcpClient client;
  client.SetResponse("MCP error: timeout");

  ToolDefinition def;
  def.name = "slow_tool";
  def.description = "A slow tool";
  def.parameters_schema = "{}";

  McpTool tool(&client, def, "mcp");

  nlohmann::json args;
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j.value("success") == false);
  REQUIRE(j.value("error") == "MCP error: timeout");
  REQUIRE(j.value("exit_code") == 1);
}

TEST_CASE("McpTool wraps MCP call error: output as error", "[mcp_tool]") {
  StubMcpClient client;
  client.SetResponse("MCP call error: connection refused");

  ToolDefinition def;
  def.name = "broken_tool";
  def.description = "Broken tool";
  def.parameters_schema = "{}";

  McpTool tool(&client, def, "mcp");

  nlohmann::json args;
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j.value("success") == false);
  REQUIRE(j.value("error") == "MCP call error: connection refused");
  REQUIRE(j.value("exit_code") == 1);
}

TEST_CASE("McpTool returns error JSON when client is null", "[mcp_tool]") {
  ToolDefinition def;
  def.name = "test";
  def.description = "test";
  def.parameters_schema = "{}";

  McpTool tool(nullptr, def, "mcp");

  nlohmann::json args;
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j.value("success") == false);
  REQUIRE(j.value("error") == "MCP client is null");
  REQUIRE(j.value("exit_code") == -1);
}

TEST_CASE("McpTool returns error JSON when client is not connected",
          "[mcp_tool]") {
  StubMcpClient client;
  client.SetConnected(false);

  ToolDefinition def;
  def.name = "test";
  def.description = "test";
  def.parameters_schema = "{}";

  McpTool tool(&client, def, "mcp");

  nlohmann::json args;
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = nlohmann::json::parse(result);
  REQUIRE(j.value("success") == false);
  REQUIRE(j.value("error") == "MCP client is not connected");
  REQUIRE(j.value("exit_code") == -1);
}