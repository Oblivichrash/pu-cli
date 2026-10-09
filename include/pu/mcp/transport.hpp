// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>

namespace pu::mcp {

using MessageCallback = std::function<void(const std::string&)>;

// Shared by StdioTransport and HttpTransport; the client depends only on this, so it is
// named after the abstraction rather than either transport.
class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool Start(MessageCallback on_message) = 0;
  virtual void Stop() = 0;
  virtual bool WriteLine(const std::string& line) = 0;
};

}  // namespace pu::mcp
