// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// Abstract HTTP client interface for dependency injection.

#ifndef PU_HTTP_HTTP_CLIENT_HPP
#define PU_HTTP_HTTP_CLIENT_HPP

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
};

}  // namespace pu::http

#endif  // PU_HTTP_HTTP_CLIENT_HPP
