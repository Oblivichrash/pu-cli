// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "pu/core/base.hpp"

namespace pu::http {

using WriteCallback = std::function<size_t(char*, size_t)>;

struct Response {
  unsigned status = 0;
  std::string body;
};

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  virtual void PostStream(const std::string& url, const std::string& body,
                          const std::vector<std::string>& headers, WriteCallback write_cb,
                          CancelToken cancel_token = nullptr) = 0;

  // For a read that is answered in one piece rather than streamed. Throws HttpError
  // on a non-2xx status, as PostStream does.
  virtual Response Get(const std::string& url, const std::vector<std::string>& headers) = 0;
};

}  // namespace pu::http
