// SPDX-License-Identifier: GPL-3.0-only
//
// Abstract HTTP client interface for dependency injection.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace pu::http {

using WriteCallback = std::function<size_t(char* ptr, size_t size)>;

class HttpClient {
 public:
  virtual ~HttpClient() = default;

  virtual void PostStream(const std::string& url,
                          const std::string& body,
                          const std::vector<std::string>& headers,
                          WriteCallback write_cb) = 0;

  virtual void SetInterruptChecker([[maybe_unused]] std::function<bool()> checker) {}
};

}  // namespace pu::http
