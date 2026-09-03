// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "pu/llm/llm_provider.hpp"

namespace pu::mcp {

struct McpServerConfig {
  std::string name;
  std::string command;
  std::vector<std::string> args;
  // Remote HTTP (streamable) MCP endpoint. When non-empty, the client uses
  // HttpTransport instead of the stdio subprocess transport.
  std::string url;
  // Optional HTTP headers sent with every request (e.g. Authorization).
  std::map<std::string, std::string> headers;
};

class McpClient {
public:
  explicit McpClient(const McpServerConfig& config);
  virtual ~McpClient();

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  bool Connect();
  void Disconnect();

  virtual std::vector<ToolDefinition> ListTools();
  virtual std::string CallTool(const std::string& name, const nlohmann::json& arguments);
  virtual bool IsConnected() const;

private:
  bool Handshake();
  nlohmann::json SendRequest(const std::string& method, const nlohmann::json& params = nullptr, int timeout_ms = 5000);

  struct Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace pu::mcp
