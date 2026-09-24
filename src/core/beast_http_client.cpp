// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/beast_http_client.hpp"

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/platform.hpp"

#include <boost/beast/version.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <regex>
#include <thread>

namespace pu::http {

BeastHttpClient::BeastHttpClient() : ssl_ctx_(net::ssl::context::tlsv12_client) {
  ssl_ctx_.set_default_verify_paths();
  ssl_ctx_.set_verify_mode(net::ssl::verify_peer);
}

BeastHttpClient::~BeastHttpClient() { ioc_.stop(); }

void BeastHttpClient::SetInterruptChecker(std::function<bool()> checker) {
  interrupt_checker_ = std::move(checker);
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
void ApplyHeaders(Request& req, const std::string& host, const std::string& body,
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
// chunks. Returns the HTTP status code, or throws HttpError on failure. On a
// failure status the body is placed in `error_body` rather than streamed, since
// the consumer parses a success stream and would discard a message that says what
// went wrong.
template <typename Stream>
unsigned StreamResponse(Stream& stream, beast::flat_buffer& buffer, WriteCallback& write_cb,
                        std::string& error_body, CancelToken /*cancel_token*/) {
  beast::http::response_parser<beast::http::string_body> parser;
  parser.body_limit(64 * 1024 * 1024);  // 64 MiB safety cap.

  beast::error_code ec;
  beast::http::read(stream, buffer, parser, ec);
  if (ec) {
    throw HttpError("HTTP read error: " + ec.message());
  }

  const unsigned status = parser.get().result_int();
  auto& res_body = parser.get().body();
  if (res_body.empty()) return status;

  if (status >= 400) {
    error_body = res_body;
    return status;
  }

  const size_t consumed = write_cb(res_body.data(), res_body.size());
  if (consumed == 0) {
    throw HttpError("Streaming aborted by consumer");
  }
  return status;
}

}  // namespace

namespace {

// A failure response explains itself, but usually as nested JSON, and it may be
// in the producer's locale. This pulls out the text a user can act on.
std::string SummarizeErrorBody(const std::string& body) {
  if (body.empty()) return "";

  const std::string text = platform::FromPipedOutput(body);
  std::string message;
  try {
    const boost::json::value parsed = boost::json::parse(text);
    // OpenAI-shaped: {"error": {"message": "..."}} or {"error": "..."}.
    const boost::json::value& error =
        json::ValueOrDefault<boost::json::value>(parsed, "error", boost::json::value{});
    if (error.is_object()) {
      message = json::ValueOrDefault<std::string>(error, "message", "");
    } else if (error.is_string()) {
      message = boost::json::value_to<std::string>(error);
    }
    if (message.empty()) message = json::ValueOrDefault<std::string>(parsed, "message", "");
  } catch (const std::exception&) {
    // Not JSON, so the body is the message.
  }
  if (message.empty()) message = text;

  // Collapse to one line so a multi-line body cannot break the log layout.
  std::string one_line;
  one_line.reserve(message.size());
  bool pending_space = false;
  for (char c : message) {
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      pending_space = !one_line.empty();
      continue;
    }
    if (pending_space) one_line += ' ';
    pending_space = false;
    one_line += c;
  }

  constexpr std::size_t kMaxDetail = 400;
  if (one_line.size() <= kMaxDetail) return one_line;
  return one_line.substr(0, kMaxDetail) + "...";
}

}  // namespace

void BeastHttpClient::PostStream(const std::string& url, const std::string& body,
                                 const std::vector<std::string>& headers, WriteCallback write_cb,
                                 CancelToken cancel_token) {
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
    std::string error_body;

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
      status = StreamResponse(ssl_stream, buffer, write_cb, error_body, cancel_token);

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
      status = StreamResponse(stream, buffer, write_cb, error_body, cancel_token);

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
      const std::string summary = SummarizeErrorBody(error_body);
      if (!summary.empty()) detail += ": " + summary;
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