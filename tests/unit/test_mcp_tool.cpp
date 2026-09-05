// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/mcp_tool.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/json.hpp>

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
                       const boost::json::value& /*arguments*/) override {
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

  boost::json::value args = boost::json::object{};
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("stdout") == "Hello from MCP server");
  REQUIRE(j.at("stderr") == "");
  REQUIRE(j.at("error") == "");
  REQUIRE(j.at("exit_code") == 0);
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

  boost::json::value args = boost::json::object{};
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("stdout") == "Error: something went wrong");
  REQUIRE(j.at("error") == "Error: something went wrong");
  REQUIRE(j.at("exit_code") == 1);
}

TEST_CASE("McpTool wraps MCP error: output as error", "[mcp_tool]") {
  StubMcpClient client;
  client.SetResponse("MCP error: timeout");

  ToolDefinition def;
  def.name = "slow_tool";
  def.description = "A slow tool";
  def.parameters_schema = "{}";

  McpTool tool(&client, def, "mcp");

  boost::json::value args = boost::json::object{};
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "MCP error: timeout");
  REQUIRE(j.at("exit_code") == 1);
}

TEST_CASE("McpTool wraps MCP call error: output as error", "[mcp_tool]") {
  StubMcpClient client;
  client.SetResponse("MCP call error: connection refused");

  ToolDefinition def;
  def.name = "broken_tool";
  def.description = "Broken tool";
  def.parameters_schema = "{}";

  McpTool tool(&client, def, "mcp");

  boost::json::value args = boost::json::object{};
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "MCP call error: connection refused");
  REQUIRE(j.at("exit_code") == 1);
}

TEST_CASE("McpTool returns error JSON when client is null", "[mcp_tool]") {
  ToolDefinition def;
  def.name = "test";
  def.description = "test";
  def.parameters_schema = "{}";

  McpTool tool(nullptr, def, "mcp");

  boost::json::value args = boost::json::object{};
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "MCP client is null");
  REQUIRE(j.at("exit_code") == -1);
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

  boost::json::value args = boost::json::object{};
  pu::ToolContext ctx;
  std::string result = tool.Execute(args, ctx);

  auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "MCP client is not connected");
  REQUIRE(j.at("exit_code") == -1);
}