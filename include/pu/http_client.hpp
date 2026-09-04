// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pu {

// Shared cancellation token used to abort an in-flight LLM request at the
// transport layer.
using CancelToken = std::shared_ptr<std::atomic<bool>>;

}  // namespace pu

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
