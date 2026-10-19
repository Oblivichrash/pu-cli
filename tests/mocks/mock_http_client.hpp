// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "pu/core/http_client.hpp"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pu::tests {

class MockHttpClient : public pu::http::HttpClient {
 public:
  void StreamRequest(pu::http::HttpMethod method, const std::string& url,
                     const std::string& body, const std::vector<std::string>& headers,
                     pu::http::WriteCallback write_cb,
                     pu::CancelToken cancel_token = nullptr) override {
    last_method = method;
    last_url = url;
    last_body = body;
    last_headers = headers;
    last_cancel_token = cancel_token;
    if (simulate_response) {
      simulate_response(url, body, headers, write_cb);
    }
  }

  pu::http::Response Get(const std::string& url,
                         const std::vector<std::string>& headers) override {
    last_url = url;
    last_headers = headers;
    last_get_url = url;
    if (simulate_get) return simulate_get(url, headers);
    throw pu::HttpError("no Get response configured");
  }

  pu::http::HttpMethod last_method = pu::http::HttpMethod::kPost;
  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
  pu::CancelToken last_cancel_token;
  std::string last_get_url;
  std::function<pu::http::Response(const std::string&, const std::vector<std::string>&)>
      simulate_get;
  std::function<void(const std::string&, const std::string&, const std::vector<std::string>&,
                     pu::http::WriteCallback)>
      simulate_response;
};

inline std::pair<std::shared_ptr<MockHttpClient>, MockHttpClient*> MakeMockHttpClient() {
  auto mock = std::make_shared<MockHttpClient>();
  auto* ptr = mock.get();
  return {std::move(mock), ptr};
}

}  // namespace pu::tests
