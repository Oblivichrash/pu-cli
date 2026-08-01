// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace pu::http {

using WriteCallback = std::function<size_t(char*, size_t)>;

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  virtual void PostStream(const std::string& url, const std::string& body,
                          const std::vector<std::string>& headers,
                          WriteCallback write_cb) = 0;
};

}  // namespace pu::http
