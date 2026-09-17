// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "pu/core/cancel_token.hpp"

namespace pu::http {

using WriteCallback = std::function<size_t(char*, size_t)>;

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  virtual void PostStream(const std::string& url, const std::string& body,
                          const std::vector<std::string>& headers,
                          WriteCallback write_cb,
                          CancelToken cancel_token = nullptr) = 0;
};

}  // namespace pu::http
