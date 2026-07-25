// SPDX-License-Identifier: GPL-3.0-only
#include "infra/curl_http_client.hpp"

#include "infra/platform.hpp"
#include "pu/error.hpp"

#include <curl/curl.h>
#include <stdexcept>
#include <fstream>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pu::http {

static bool IsTraceEnabled() {
  static const char* env = std::getenv("PU_TRACE");
  return env && (std::string(env) == "1" || std::string(env) == "true");
}

static std::ofstream& GetTraceLog() {
  static std::ofstream log;
  static std::once_flag flag;
  std::call_once(flag, []() {
    const char* path = std::getenv("PU_TRACE_LOG");
    if (!path) path = "/tmp/pu_trace.jsonl";
    log.open(path, std::ios::app);
  });
  return log;
}

CurlSlist::~CurlSlist() { if (list) curl_slist_free_all(list); }
void CurlSlist::append(const char* str) { list = curl_slist_append(list, str); }

CurlHttpClient::CurlHttpClient() {
  handle_ = curl_easy_init();
  if (!handle_) throw pu::Error("Failed to initialize libcurl");
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

  if (IsTraceEnabled()) {
    nlohmann::json req_log;
    req_log["trace_id"] = trace_id;
    req_log["timestamp"] = std::chrono::system_clock::now().time_since_epoch().count();
    req_log["url"] = url;
    req_log["body"] = body;
    req_log["method"] = "POST";
    GetTraceLog() << req_log.dump() << std::endl;
  }

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

  if (IsTraceEnabled()) {
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    nlohmann::json resp_log;
    resp_log["trace_id"] = trace_id;
    resp_log["duration_ms"] = duration_ms;
    resp_log["http_code"] = http_code;

    GetTraceLog() << resp_log.dump() << std::endl;
  }

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
