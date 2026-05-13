// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <vector>
#include <system_error>

namespace pu::http {

using WriteCallback = std::function<size_t(char*, size_t)>;

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  virtual void PostStream(const std::string& url, const std::string& body,
                          const std::vector<std::string>& headers,
                          WriteCallback write_cb, std::error_code& ec) = 0;
  virtual void SetInterruptChecker([[maybe_unused]] std::function<bool()> checker) {}
};

}  // namespace pu::http
