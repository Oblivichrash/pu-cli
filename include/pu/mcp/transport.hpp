// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>

namespace pu::mcp {

using MessageCallback = std::function<void(const std::string&)>;

class Transport {
 public:
  virtual ~Transport() = default;
  virtual bool Start(MessageCallback on_message) = 0;
  virtual void Stop() = 0;
  virtual bool WriteLine(const std::string& line) = 0;
};

}  // namespace pu::mcp
