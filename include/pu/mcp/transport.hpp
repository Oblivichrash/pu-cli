// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>

namespace pu::mcp {

using MessageCallback = std::function<void(const std::string&)>;

// Common transport interface shared by StdioTransport and the remote
// HttpTransport. McpClient/JsonRpcClient depend only on this interface, so it
// is named after the abstraction rather than either concrete transport.
class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool Start(MessageCallback on_message) = 0;
  virtual void Stop() = 0;
  virtual bool WriteLine(const std::string& line) = 0;
};

}  // namespace pu::mcp
