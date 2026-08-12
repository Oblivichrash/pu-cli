// SPDX-License-Identifier: GPL-3.0-only
#include "infra/curl_http_client.hpp"

#include "pu/core/logging.hpp"
#include "pu/infra/platform.hpp"
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

void ThrowOnFailure(CURLcode res, long http_code, const std::string& response_body,
                    std::string& error_detail) {
  if (res != CURLE_OK) {
    std::string detail = "CURL error " + std::to_string(res) + ": " + curl_easy_strerror(res);
    if (!response_body.empty()) {
      detail += "\nResponse: " + response_body.substr(0, 1024);
    }
    error_detail = detail;
    throw HttpError(detail);
  }
  if (http_code >= 400) {
    std::string detail = "HTTP error " + std::to_string(http_code);
    if (!response_body.empty()) {
      detail += "\nResponse: " + response_body.substr(0, 1024);
    }
    error_detail = detail;
    throw HttpError(detail);
  }
}

void ApplyCommonOptions(CURL* handle) {
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "pu-cli/1.0");
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
}

}  // namespace

void CurlHttpClient::PostStream(const std::string& url, const std::string& body,
                                const std::vector<std::string>& headers,
                                WriteCallback write_cb) {
  auto start = std::chrono::steady_clock::now();
  std::string trace_id = std::to_string(start.time_since_epoch().count());

  spdlog::trace("HTTP POST {} body_size={}", url, body.size());

  std::string error_detail;
  response_body_.clear();

  curl_easy_reset(handle_);

  curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle_, CURLOPT_POST, 1L);
  curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));

  ApplyCommonOptions(handle_);

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
  SetLogDurationMs(duration_ms);
  spdlog::trace("HTTP {} {} {}ms", url, http_code, duration_ms);
  ClearLogDurationMs();

  ThrowOnFailure(res, http_code, response_body_, error_detail);
}

std::string CurlHttpClient::Get(const std::string& url,
                                const std::vector<std::string>& headers) {
  spdlog::trace("HTTP GET {}", url);

  std::string error_detail;
  response_body_.clear();

  curl_easy_reset(handle_);

  curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle_, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(handle_, CURLOPT_POST, 0L);
  curl_easy_setopt(handle_, CURLOPT_NOBODY, 0L);

  ApplyCommonOptions(handle_);

  CurlSlist slist;
  for (const auto& h : headers) slist.append(h.c_str());
  curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, slist.list);

  WriteContext ctx{[](char*, size_t n) { return n; }, &response_body_};
  curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, WriteCallbackTrampoline);
  curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(handle_, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle_, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
  curl_easy_setopt(handle_, CURLOPT_XFERINFODATA, this);

  CURLcode res = curl_easy_perform(handle_);
  long http_code = 0;
  curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &http_code);

  spdlog::trace("HTTP {} {} bytes={}", url, http_code, response_body_.size());

  ThrowOnFailure(res, http_code, response_body_, error_detail);
  return response_body_;
}

}  // namespace pu::http
