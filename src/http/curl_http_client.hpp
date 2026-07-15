// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/http/http_client.hpp"
#include <curl/curl.h>
#include <functional>
#include <string>

namespace pu::http {

class CurlHttpClient : public HttpClient {
 public:
  CurlHttpClient();
  ~CurlHttpClient() override;
  CurlHttpClient(const CurlHttpClient&) = delete;
  CurlHttpClient& operator=(const CurlHttpClient&) = delete;

  void PostStream(const std::string& url, const std::string& body,
                  const std::vector<std::string>& headers, WriteCallback write_cb) override;
  void SetInterruptChecker(std::function<bool()> checker) override;

 private:
  CURL* handle_;
  std::function<bool()> interrupt_checker_;
  static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow);
};

struct CurlSlist {
  struct curl_slist* list = nullptr;
  ~CurlSlist();
  void append(const char* str);
  operator struct curl_slist*() const { return list; }
};

}  // namespace pu::http
