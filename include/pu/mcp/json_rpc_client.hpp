// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <future>
#include <mutex>
#include <unordered_map>

#include <boost/json.hpp>

#include "pu/mcp/stdio_transport.hpp"

namespace pu::mcp {

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

}  // namespace pu::mcp
