// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#ifndef PU_TESTS_MOCKS_MOCK_HTTP_CLIENT_HPP
#define PU_TESTS_MOCKS_MOCK_HTTP_CLIENT_HPP

#include "pu/http/http_client.hpp"
#include <string>

namespace pu::tests {

class MockHttpClient : public pu::http::HttpClient {
 public:
  void PostStream(const std::string& url,
                  const std::string& body,
                  const std::vector<std::string>& headers,
                  WriteCallback write_cb) override {
    last_url = url;
    last_body = body;
    last_headers = headers;
    if (simulate_response) {
      simulate_response(write_cb);
    }
  }

  std::string last_url;
  std::string last_body;
  std::vector<std::string> last_headers;
  std::function<void(WriteCallback&)> simulate_response;
};

}  // namespace pu::tests

#endif  // PU_TESTS_MOCKS_MOCK_HTTP_CLIENT_HPP
