// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/json.hpp>

#include "pu/llm/llm_provider.hpp"
#include "pu/mcp/transport.hpp"

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

// JSON-RPC 2.0 over a transport: one promise per request id, answered by the
// reply that carries the same id.
class JsonRpcClient {
public:
  explicit JsonRpcClient(Transport& transport);
  ~JsonRpcClient() = default;

  // Send a request, returning a future for the async response.
  std::future<boost::json::value> SendRequest(const std::string& method,
                                             const boost::json::value& params = {});

  // Handle incoming messages (called by transport callback).
  void OnMessage(const std::string& line);

private:
  Transport& transport_;
  int next_id_ = 1;
  std::unordered_map<int, std::promise<boost::json::value>> pending_;
  std::mutex mutex_;
};

// The MCP surface the rest of pu-cli uses: handshake, tool listing, tool calls.
class McpClient {
public:
  explicit McpClient(const McpServerConfig& config);
  virtual ~McpClient();

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  bool Connect();
  void Disconnect();

  virtual std::vector<ToolDefinition> ListTools();
  virtual std::string CallTool(const std::string& name,
                               const boost::json::value& arguments);
  virtual bool IsConnected() const;

private:
  bool Handshake();
  boost::json::value SendRequest(const std::string& method,
                                const boost::json::value& params = {},
                                int timeout_ms = 5000);

  struct Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace pu::mcp
