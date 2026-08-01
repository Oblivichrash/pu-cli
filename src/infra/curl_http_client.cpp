// SPDX-License-Identifier: GPL-3.0-only
#include "infra/curl_http_client.hpp"

#include "infra/platform.hpp"
#include "pu/error.hpp"

#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <fstream>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pu::http {

CurlSlist::~CurlSlist() { if (list) curl_slist_free_all(list); }
void CurlSlist::append(const char* str) { list = curl_slist_append(list, str); }

CurlHttpClient::CurlHttpClient() {
  handle_ = curl_easy_init();
  if (!handle_) throw pu::Error("Failed to initialize libcurl");
  interrupt_checker_ = [] { return pu::platform::IsInterrupted(); };
}

CurlHttpClient::~CurlHttpClient() { if (handle_) curl_easy_cleanup(handle_); }

int CurlHttpClient::ProgressCallback(void* clientp, curl_off_t, curl_off_t,
                                     curl_off_t, curl_off_t) {
  auto* self = static_cast<CurlHttpClient*>(clientp);
  if (self->interrupt_checker_ && self->interrupt_checker_()) return 1;
  return 0;
}

namespace {

struct WriteContext {
  WriteCallback cb;
  std::string* body;
};

size_t WriteCallbackTrampoline(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<WriteContext*>(userdata);
  size_t bytes = size * nmemb;
  if (ctx->body) ctx->body->append(ptr, bytes);
  return ctx->cb(ptr, bytes);
}

}  // namespace

void CurlHttpClient::PostStream(const std::string& url, const std::string& body,
                                const std::vector<std::string>& headers,
                                WriteCallback write_cb) {
  auto start = std::chrono::steady_clock::now();
  std::string trace_id = std::to_string(start.time_since_epoch().count());

  spdlog::trace("HTTP POST {} body_size={}", url, body.size());

  error_detail_.clear();
  response_body_.clear();

  curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));

  CurlSlist slist;
  for (const auto& h : headers) slist.append(h.c_str());
  curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, slist.list);

  WriteContext ctx{write_cb, &response_body_};
  curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, WriteCallbackTrampoline);
  curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(handle_, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle_, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(handle_, CURLOPT_XFERINFODATA, this);

  CURLcode res = curl_easy_perform(handle_);
  long http_code = 0;
  curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &http_code);

  auto end = std::chrono::steady_clock::now();
  auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  spdlog::trace("HTTP {} {} {}ms", url, http_code, duration_ms);

  if (res != CURLE_OK) {
    std::string detail = "CURL error " + std::to_string(res) + ": " + curl_easy_strerror(res);
    if (!response_body_.empty()) {
      detail += "\nResponse: " + response_body_.substr(0, 1024);
    }
    error_detail_ = detail;
    throw HttpError(detail);
  }
  if (http_code >= 400) {
    std::string detail = "HTTP error " + std::to_string(http_code);
    if (!response_body_.empty()) {
      detail += "\nResponse: " + response_body_.substr(0, 1024);
    }
    error_detail_ = detail;
    throw HttpError(detail);
  }
  curl_easy_reset(handle_);
}

std::string CurlHttpClient::GetErrorDetail() const { return error_detail_; }

}  // namespace pu::http