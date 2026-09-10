// SPDX-License-Identifier: GPL-3.0-only
#include "infra/beast_http_client.hpp"

#include "pu/core/error.hpp"
#include "pu/core/logging.hpp"

#include <boost/beast/version.hpp>
#include <spdlog/spdlog.h>

#include <regex>
#include <thread>

namespace pu::http {

BeastHttpClient::BeastHttpClient()
    : ssl_ctx_(net::ssl::context::tlsv12_client) {
  ssl_ctx_.set_default_verify_paths();
  ssl_ctx_.set_verify_mode(net::ssl::verify_peer);
}

BeastHttpClient::~BeastHttpClient() {
  ioc_.stop();
}

void BeastHttpClient::SetInterruptChecker(std::function<bool()> checker) {
  interrupt_checker_ = std::move(checker);
}

std::string BeastHttpClient::GetErrorDetail() const {
  return error_detail_;
}

void BeastHttpClient::CheckCancel(CancelToken token) const {
  if (token && token->load(std::memory_order_acquire)) {
    throw HttpError("Request cancelled");
  }
  if (interrupt_checker_ && interrupt_checker_()) {
    throw HttpError("Interrupted by signal");
  }
}

BeastHttpClient::UrlParts BeastHttpClient::ParseUrl(const std::string& url) const {
  UrlParts parts;
  std::regex url_regex(R"(^([a-zA-Z]+)://([^:/]+)(?::([0-9]+))?(/.*)?$)");
  std::smatch match;
  if (!std::regex_match(url, match, url_regex)) {
    throw HttpError("Invalid URL: " + url);
  }
  parts.scheme = match[1];
  parts.host = match[2];
  parts.port = match[3].matched ? match[3].str() : (parts.scheme == "https" ? "443" : "80");
  parts.target = match[4].matched ? match[4].str() : "/";
  return parts;
}

namespace {

template <typename Request>
void ApplyHeaders(Request& req, const std::string& host,
                  const std::string& body,
                  const std::vector<std::string>& headers) {
  req.set(beast::http::field::host, host);
  req.set(beast::http::field::content_type, "application/json");
  req.set(beast::http::field::content_length, std::to_string(body.size()));
  for (const auto& h : headers) {
    size_t pos = h.find(':');
    if (pos != std::string::npos) {
      std::string name = h.substr(0, pos);
      std::string value = h.substr(pos + 1);
      // Trim leading whitespace from value.
      size_t first = value.find_first_not_of(" \t");
      if (first != std::string::npos) value.erase(0, first);
      req.set(name, value);
    }
  }
  req.body() = body;
}

// Reads the full response into a string body, then streams it to write_cb in
// chunks. Returns the HTTP status code, or throws HttpError on failure.
template <typename Stream>
unsigned StreamResponse(Stream& stream, beast::flat_buffer& buffer,
                        WriteCallback& write_cb,
                        CancelToken /*cancel_token*/) {
  beast::http::response_parser<beast::http::string_body> parser;
  parser.body_limit(64 * 1024 * 1024);  // 64 MiB safety cap.

  beast::error_code ec;
  beast::http::read(stream, buffer, parser, ec);
  if (ec) {
    throw HttpError("HTTP read error: " + ec.message());
  }

  unsigned status = parser.get().result_int();
  auto& res_body = parser.get().body();
  if (!res_body.empty()) {
    size_t consumed = write_cb(res_body.data(), res_body.size());
    if (consumed == 0) {
      throw HttpError("Streaming aborted by consumer");
    }
  }
  return status;
}

}  // namespace

void BeastHttpClient::PostStream(const std::string& url,
                                 const std::string& body,
                                 const std::vector<std::string>& headers,
                                 WriteCallback write_cb,
                                 CancelToken cancel_token) {
  error_detail_.clear();
  auto start = std::chrono::steady_clock::now();

  try {
    CheckCancel(cancel_token);

    auto parts = ParseUrl(url);
    bool use_ssl = parts.scheme == "https";

    tcp::resolver resolver(ioc_);
    beast::tcp_stream stream(ioc_);

    auto endpoints = resolver.resolve(parts.host, parts.port);
    CheckCancel(cancel_token);

    stream.connect(endpoints);
    CheckCancel(cancel_token);

    unsigned status = 0;

    if (use_ssl) {
      beast::ssl_stream<beast::tcp_stream> ssl_stream(std::move(stream), ssl_ctx_);
      ssl_stream.handshake(net::ssl::stream_base::client);
      CheckCancel(cancel_token);

      beast::http::request<beast::http::string_body> req(beast::http::verb::post, parts.target, 11);
      ApplyHeaders(req, parts.host, body, headers);

      CheckCancel(cancel_token);
      beast::http::write(ssl_stream, req);
      CheckCancel(cancel_token);

      beast::flat_buffer buffer;
      status = StreamResponse(ssl_stream, buffer, write_cb, cancel_token);

      auto end = std::chrono::steady_clock::now();
      auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      SetLogDurationMs(duration_ms);
      spdlog::trace("HTTP {} {} {}ms", url, status, duration_ms);
      ClearLogDurationMs();

      beast::error_code ec;
      ssl_stream.shutdown(ec);
    } else {
      beast::http::request<beast::http::string_body> req(beast::http::verb::post, parts.target, 11);
      ApplyHeaders(req, parts.host, body, headers);

      CheckCancel(cancel_token);
      beast::http::write(stream, req);
      CheckCancel(cancel_token);

      beast::flat_buffer buffer;
      status = StreamResponse(stream, buffer, write_cb, cancel_token);

      auto end = std::chrono::steady_clock::now();
      auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      SetLogDurationMs(duration_ms);
      spdlog::trace("HTTP {} {} {}ms", url, status, duration_ms);
      ClearLogDurationMs();

      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    }

    if (status >= 400) {
      std::string detail = "HTTP error " + std::to_string(status);
      error_detail_ = detail;
      throw HttpError(detail);
    }

  } catch (const std::exception& e) {
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    SetLogDurationMs(duration_ms);
    spdlog::trace("HTTP {} error: {} {}ms", url, e.what(), duration_ms);
    ClearLogDurationMs();
    throw;
  }
}

}  // namespace pu::http