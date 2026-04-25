// SPDX-License-Identifier: GPL-3.0-only
//
// libcurl implementation of HttpClient.

#pragma once

#include "pu/http/http_client.hpp"
#include <curl/curl.h>
#include <functional>
#include <stdexcept>
#include <string>

namespace pu::http {

class CurlHttpClient : public HttpClient {
 public:
  CurlHttpClient();
  ~CurlHttpClient() override;

  CurlHttpClient(const CurlHttpClient&) = delete;
  CurlHttpClient& operator=(const CurlHttpClient&) = delete;

  void PostStream(const std::string& url,
                  const std::string& body,
                  const std::vector<std::string>& headers,
                  WriteCallback write_cb) override;

 private:
  CURL* handle_;
};

struct CurlSlist {
  struct curl_slist* list = nullptr;
  ~CurlSlist();
  void append(const char* str);
  operator struct curl_slist*() const { return list; }
};

}  // namespace pu::http
