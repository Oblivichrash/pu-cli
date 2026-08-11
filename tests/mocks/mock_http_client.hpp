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
                  pu::http::WriteCallback write_cb) override {
    last_url = url;
    last_body = body;
    last_headers = headers;
    if (simulate_response) {
      simulate_response(url, body, headers, write_cb);
    }
  }

  std::string Get(const std::string& url,
                  const std::vector<std::string>& headers = {}) override {
    last_get_url = url;
    last_get_headers = headers;
    if (simulate_get_response) {
      return simulate_get_response(url, headers);
    }
    return get_response;
  }

  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
  std::string last_get_url;
  std::vector<std::string> last_get_headers;
  std::string get_response;
  std::function<void(const std::string&,
                     const std::string&,
                     const std::vector<std::string>&,
                     pu::http::WriteCallback)> simulate_response;
  std::function<std::string(const std::string&, const std::vector<std::string>&)>
      simulate_get_response;
};

}  // namespace pu::tests
