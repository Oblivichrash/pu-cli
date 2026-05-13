// SPDX-License-Identifier: GPL-3.0-only
#include "curl_http_client.hpp"
#include "platform/platform.hpp"
#include "pu/error_codes.hpp"
#include <curl/curl.h>
#include <stdexcept>

namespace pu::http {

CurlSlist::~CurlSlist() { if (list) curl_slist_free_all(list); }
void CurlSlist::append(const char* str) { list = curl_slist_append(list, str); }

CurlHttpClient::CurlHttpClient() {
  handle_ = curl_easy_init();
  if (!handle_) throw std::runtime_error("Failed to initialize libcurl");
  interrupt_checker_ = [] { return pu::platform::IsInterrupted(); };
}

CurlHttpClient::~CurlHttpClient() { if (handle_) curl_easy_cleanup(handle_); }

void CurlHttpClient::SetInterruptChecker(std::function<bool()> checker) {
  interrupt_checker_ = std::move(checker);
}

int CurlHttpClient::ProgressCallback(void* clientp, curl_off_t, curl_off_t,
                                     curl_off_t, curl_off_t) {
  auto* self = static_cast<CurlHttpClient*>(clientp);
  if (self->interrupt_checker_ && self->interrupt_checker_()) return 1;
  return 0;
}

static size_t WriteCallbackTrampoline(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto& cb = *static_cast<WriteCallback*>(userdata);
  return cb(ptr, size * nmemb);
}

void CurlHttpClient::PostStream(const std::string& url, const std::string& body,
                                const std::vector<std::string>& headers,
                                WriteCallback write_cb, std::error_code& ec) {
  ec.clear();
  curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));

  CurlSlist slist;
  for (const auto& h : headers) slist.append(h.c_str());
  curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, slist.list);
  curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, WriteCallbackTrampoline);
  curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &write_cb);
  curl_easy_setopt(handle_, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle_, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(handle_, CURLOPT_XFERINFODATA, this);

  CURLcode res = curl_easy_perform(handle_);
  if (res != CURLE_OK) {
    ec = (interrupt_checker_ && interrupt_checker_()) ? HttpErrc::interrupted
                                                      : HttpErrc::connection_failed;
    curl_easy_reset(handle_);
    return;
  }
  long http_code = 0;
  curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code >= 400) ec = HttpErrc::http_error;
  curl_easy_reset(handle_);
}

}  // namespace pu::http
