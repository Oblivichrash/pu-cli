// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.
//
// libcurl implementation of HttpClient.

#ifndef PU_HTTP_CURL_HTTP_CLIENT_HPP
#define PU_HTTP_CURL_HTTP_CLIENT_HPP

#include "pu/http/http_client.hpp"
#include <curl/curl.h>
#include <stdexcept>
#include <string>

namespace pu::http {

class CurlHttpClient : public HttpClient {
 public:
  CurlHttpClient();
  ~CurlHttpClient() override;

  // Non-copyable
  CurlHttpClient(const CurlHttpClient&) = delete;
  CurlHttpClient& operator=(const CurlHttpClient&) = delete;

  void PostStream(const std::string& url,
                  const std::string& body,
                  const std::vector<std::string>& headers,
                  WriteCallback write_cb) override;

 private:
  CURL* handle_;
};

// RAII wrapper for curl_slist (used internally)
struct CurlSlist {
  struct curl_slist* list = nullptr;
  ~CurlSlist();
  void append(const char* str);
  operator struct curl_slist*() const { return list; }
};

}  // namespace pu::http

#endif  // PU_HTTP_CURL_HTTP_CLIENT_HPP
