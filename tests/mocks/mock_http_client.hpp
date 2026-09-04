// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "pu/http_client.hpp"
#include <functional>
#include <string>
#include <vector>

namespace pu::tests {

class MockHttpClient : public pu::http::HttpClient {
 public:
  void PostStream(const std::string& url,
                  const std::string& body,
                  const std::vector<std::string>& headers,
                  pu::http::WriteCallback write_cb,
                  pu::CancelToken cancel_token = nullptr) override {
    last_url = url;
    last_body = body;
    last_headers = headers;
    last_cancel_token = cancel_token;
    if (simulate_response) {
      simulate_response(url, body, headers, write_cb);
    }
  }

  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
  pu::CancelToken last_cancel_token;
  std::function<void(const std::string&,
                     const std::string&,
                     const std::vector<std::string>&,
                     pu::http::WriteCallback)> simulate_response;
};

}  // namespace pu::tests
