// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/mcp_server.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/mcp/transport.hpp"

namespace pu::mcp {

class JsonRpcClient {
 public:
  explicit JsonRpcClient(Transport& transport);
  ~JsonRpcClient() = default;

  std::future<boost::json::value> SendRequest(const std::string& method,
                                              const boost::json::value& params = {});

  void OnMessage(const std::string& line);

 private:
  Transport& transport_;
  int next_id_ = 1;
  std::unordered_map<int, std::promise<boost::json::value>> pending_;
  std::mutex mutex_;
};

class McpClient {
 public:
  explicit McpClient(const config::McpServerConfig& config);
  virtual ~McpClient();

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  bool Connect();
  void Disconnect();

  virtual std::vector<ToolDefinition> ListTools();
  virtual std::string CallTool(const std::string& name, const boost::json::value& arguments);
  virtual bool IsConnected() const;

  const std::string& ServerName() const;

 private:
  bool Handshake();
  boost::json::value SendRequest(const std::string& method, const boost::json::value& params = {},
                                 int timeout_ms = 5000);

  struct Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace pu::mcp
