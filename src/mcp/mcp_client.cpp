// SPDX-License-Identifier: GPL-3.0-only
#include "pu/mcp/mcp_client.hpp"
#include "pu/mcp/stdio_transport.hpp"
#include "mcp/http_transport.hpp"
#include "pu/mcp/json_rpc_client.hpp"
#include "pu/error.hpp"
#include "pu/json.hpp"
#include <spdlog/spdlog.h>
#include <future>
#include <chrono>

namespace pu::mcp {

struct McpClient::Impl {
  McpServerConfig config;
  std::unique_ptr<Transport> transport;
  std::unique_ptr<JsonRpcClient> rpc;
  bool connected = false;
  std::vector<ToolDefinition> cached_tools;
};

McpClient::McpClient(const McpServerConfig& config)
    : pimpl_(std::make_unique<Impl>()) {
  pimpl_->config = config;
}

McpClient::~McpClient() { Disconnect(); }

bool McpClient::Connect() {
  if (pimpl_->connected) return true;

  if (!pimpl_->config.url.empty()) {
    pimpl_->transport = std::make_unique<HttpTransport>(
        pimpl_->config.url, pimpl_->config.headers);
    spdlog::debug("MCP connecting via HTTP: {} ({})",
                  pimpl_->config.name, pimpl_->config.url);
  } else {
    pimpl_->transport = std::make_unique<StdioTransport>(
        pimpl_->config.command, pimpl_->config.args);
    spdlog::debug("MCP connecting via stdio: {}", pimpl_->config.name);
  }

  if (!pimpl_->transport->Start([this](const std::string& line) {
        if (pimpl_->rpc) pimpl_->rpc->OnMessage(line);
      })) {
    spdlog::error("MCP transport start failed for '{}'", pimpl_->config.name);
    return false;
  }

  pimpl_->rpc = std::make_unique<JsonRpcClient>(*pimpl_->transport);
  if (!Handshake()) {
    spdlog::error("MCP handshake failed for '{}'", pimpl_->config.name);
    Disconnect();
    return false;
  }

  pimpl_->connected = true;
  spdlog::info("MCP client connected: {}", pimpl_->config.name);
  return true;
}

void McpClient::Disconnect() {
  if (pimpl_->transport) {
    pimpl_->transport->Stop();
    pimpl_->transport.reset();
  }
  pimpl_->rpc.reset();
  pimpl_->connected = false;
  pimpl_->cached_tools.clear();
}

bool McpClient::Handshake() {
  boost::json::value init_params = {
      {"protocolVersion", "2024-11-05"},
      {"clientInfo", {{"name", "pu-cli"}, {"version", "0.3.1"}}},
      {"capabilities", {{"tools", true}}}};
  try {
    auto resp = SendRequest("initialize", init_params);
    if (!json::HasKey(resp, "result")) return false;
    // Send initialized notification (no response needed)
    pimpl_->transport->WriteLine(boost::json::serialize(boost::json::value{
        {"jsonrpc", "2.0"},
        {"method", "initialized"}}));
    return true;
  } catch (const std::exception& e) {
    spdlog::error("Handshake error: {}", e.what());
    return false;
  }
}

boost::json::value McpClient::SendRequest(const std::string& method,
                                          const boost::json::value& params,
                                          int timeout_ms) {
  // Only check that rpc exists; connected may be false during handshake.
  if (!pimpl_->rpc) {
    throw RuntimeError("MCP client not connected");
  }
  auto future = pimpl_->rpc->SendRequest(method, params);
  auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
  if (status == std::future_status::timeout) {
    throw RuntimeError("MCP request timeout");
  }
  return future.get();
}

std::vector<ToolDefinition> McpClient::ListTools() {
  if (!pimpl_->connected) {
    spdlog::warn("Cannot list tools: client not connected");
    return {};
  }
  if (!pimpl_->cached_tools.empty()) return pimpl_->cached_tools;

  try {
    auto resp = SendRequest("tools/list", {});
    if (json::HasKey(resp, "result") && json::HasKey(resp.at("result"), "tools")) {
      std::vector<ToolDefinition> defs;
      for (const auto& t : resp.at("result").at("tools").as_array()) {
        ToolDefinition def;
        def.name = json::ValueOrDefault<std::string>(t, "name", "");
        def.description = json::ValueOrDefault<std::string>(t, "description", "");
        if (json::HasKey(t, "inputSchema")) {
          def.parameters_schema = boost::json::serialize(t.at("inputSchema"));
        } else {
          def.parameters_schema = "{}";
        }
        defs.push_back(def);
      }
      pimpl_->cached_tools = defs;
      return defs;
    }
  } catch (const std::exception& e) {
    spdlog::error("ListTools failed: {}", e.what());
  }
  return {};
}

std::string McpClient::CallTool(const std::string& name,
                                const boost::json::value& arguments) {
  if (!pimpl_->connected) {
    return "Error: MCP client not connected";
  }
  try {
    boost::json::value params = {{"name", name}, {"arguments", arguments}};
    auto resp = SendRequest("tools/call", params);
    if (json::HasKey(resp, "result") && json::HasKey(resp.at("result"), "content")) {
      std::string output;
      for (const auto& item : resp.at("result").at("content").as_array()) {
        if (json::HasKey(item, "type") && item.at("type") == "text") {
          output += json::ValueOrDefault<std::string>(item, "text", "") + "\n";
        }
      }
      if (!output.empty()) return output;
      return boost::json::serialize(resp.at("result"));
    } else if (json::HasKey(resp, "error")) {
      return "MCP error: " + boost::json::serialize(resp.at("error"));
    }
    return boost::json::serialize(resp);
  } catch (const std::exception& e) {
    spdlog::error("CallTool failed: {}", e.what());
    return std::string("MCP call error: ") + e.what();
  }
}

bool McpClient::IsConnected() const { return pimpl_->connected; }

}  // namespace pu::mcp
