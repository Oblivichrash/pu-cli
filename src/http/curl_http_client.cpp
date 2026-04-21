// Copyright (c) 2026 pu-cli authors. All rights reserved.
// Use of this source code is governed by a GPL-3.0-style license that can be
// found in the LICENSE file.

#include "curl_http_client.hpp"
#include <curl/curl.h>
#include <stdexcept>

namespace pu::http {

// ============================================================================
// CurlSlist implementation
// ============================================================================
CurlSlist::~CurlSlist() {
  if (list) curl_slist_free_all(list);
}

void CurlSlist::append(const char* str) {
  list = curl_slist_append(list, str);
}

// ============================================================================
// CurlHttpClient implementation
// ============================================================================
CurlHttpClient::CurlHttpClient() {
  handle_ = curl_easy_init();
  if (!handle_) throw std::runtime_error("Failed to initialize libcurl");
}

CurlHttpClient::~CurlHttpClient() {
  if (handle_) curl_easy_cleanup(handle_);
}

void CurlHttpClient::PostStream(const std::string& url,
                                const std::string& body,
                                const std::vector<std::string>& headers,
                                WriteCallback write_cb) {
  curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(body.size()));

  CurlSlist slist;
  for (const auto& h : headers) slist.append(h.c_str());
  curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, slist.list);

  // C-style callback trampoline
  static auto trampoline = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
    auto& cb = *static_cast<WriteCallback*>(userdata);
    return cb(ptr, size * nmemb);
  };
  curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, trampoline);
  curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &write_cb);

  CURLcode res = curl_easy_perform(handle_);
  if (res != CURLE_OK) {
    throw std::runtime_error(std::string("libcurl error: ") + curl_easy_strerror(res));
  }

  long http_code = 0;
  curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code >= 400) {
    throw std::runtime_error("HTTP error: " + std::to_string(http_code));
  }

  curl_easy_reset(handle_);  // Prepare for next request
}

}  // namespace pu::http
