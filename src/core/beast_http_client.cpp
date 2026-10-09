// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/beast_http_client.hpp"

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/core/logging.hpp"
#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"

#include <boost/beast/version.hpp>
#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <regex>
#include <thread>

#ifdef _WIN32
// Windows keeps its trust anchors in the registry rather than in the Unix layout OpenSSL
// looks for, so the store is read through its own API.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <windows.h>
#include <wincrypt.h>
#endif

namespace pu::http {

#ifdef _WIN32
namespace {

// OpenSSL's default verify paths describe a Unix filesystem, so on Windows every public
// HTTPS request would fail its handshake until the roots this platform trusts are added.
void AddWindowsRootStore(net::ssl::context& ctx) {
  HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
  if (store == nullptr) {
    spdlog::warn("Could not open the Windows root store; HTTPS verification may fail");
    return;
  }

  X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());
  std::size_t added = 0;
  for (PCCERT_CONTEXT cert = nullptr;
       (cert = CertEnumCertificatesInStore(store, cert)) != nullptr;) {
    const unsigned char* der = cert->pbCertEncoded;
    X509* parsed = d2i_X509(nullptr, &der, static_cast<long>(cert->cbCertEncoded));
    if (parsed == nullptr) {
      ERR_clear_error();
      continue;
    }
    if (X509_STORE_add_cert(x509_store, parsed) == 1) {
      ++added;
    } else {
      // A certificate already in the store, which is why the error is cleared
      // rather than left for whatever asks next.
      ERR_clear_error();
    }
    X509_free(parsed);
  }
  CertCloseStore(store, 0);

  // Trusting nothing fails in a way no request explains, so it is said here rather
  // than left to the handshake.
  if (added == 0) spdlog::warn("No certificates loaded from the Windows root store");
}

}  // namespace
#endif

BeastHttpClient::BeastHttpClient() : ssl_ctx_(net::ssl::context::tlsv12_client) {
  ssl_ctx_.set_default_verify_paths();
#ifdef _WIN32
  AddWindowsRootStore(ssl_ctx_);
#endif
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
      // `Name: value` is how a header is written, so the space after the colon is
      // framing rather than part of either half.
      req.set(text::Trim(std::string_view(h).substr(0, pos)),
              text::Trim(std::string_view(h).substr(pos + 1)));
    }
  }
  req.body() = body;
}

// Hands each piece of the body to write_cb as it arrives; on a failure status the body is
// collected into `error_body` instead. `need_buffer` is how the body continues, not an error.
template <typename Stream>
unsigned StreamResponse(Stream& stream, beast::flat_buffer& buffer, WriteCallback& write_cb,
                        std::string& error_body, CancelToken cancel_token) {
  beast::http::response_parser<beast::http::buffer_body> parser;
  parser.body_limit(64 * 1024 * 1024);  // 64 MiB safety cap.

  beast::error_code ec;
  beast::http::read_header(stream, buffer, parser, ec);
  if (ec) {
    throw HttpError("HTTP read error: " + ec.message());
  }

  const unsigned status = parser.get().result_int();
  const bool failure = status >= 400;

  char piece[16 * 1024];
  std::string collected;
  while (!parser.is_done()) {
    parser.get().body().data = piece;
    parser.get().body().size = sizeof(piece);

    beast::http::read_some(stream, buffer, parser, ec);
    if (ec == beast::http::error::need_buffer) {
      ec = {};
    } else if (ec) {
      throw HttpError("HTTP read error: " + ec.message());
    }

    const std::size_t produced = sizeof(piece) - parser.get().body().size;
    if (produced == 0) continue;

    if (failure) {
      // Enough of a failure body to explain it; the rest is drained rather than
      // kept, since the summary keeps a line of it and not a transcript.
      constexpr std::size_t kMaxErrorBody = 64 * 1024;
      if (collected.size() < kMaxErrorBody) {
        collected.append(piece, std::min(produced, kMaxErrorBody - collected.size()));
      }
      continue;
    }

    const size_t consumed = write_cb(piece, produced);
    if (consumed == 0) {
      throw HttpError("Streaming aborted by consumer");
    }
    // A stop asked for while the body is arriving ends the request here: the callback is
    // reached once per read, so a quiet stream would hold a stop until the producer speaks.
    if (cancel_token && cancel_token->load(std::memory_order_acquire)) {
      throw HttpError("Request cancelled");
    }
  }

  if (failure) error_body = std::move(collected);
  return status;
}

}  // namespace

namespace {

// A failure response explains itself, but usually as nested JSON, and it may be
// in the producer's locale. This pulls out the text a user can act on.
std::string SummarizeErrorBody(const std::string& body) {
  if (body.empty()) return "";

  // The body may arrive in the producer's locale, and its shape differs by gateway:
  // the envelopes `json::ErrorMessage` reads are the ones that turn up here.
  const std::string text = platform::FromPipedOutput(body);
  std::string message;
  try {
    message = json::ErrorMessage(boost::json::parse(text));
  } catch (const std::exception&) {
    // Not JSON, so the body is the message.
  }
  if (message.empty()) message = text;

  // Collapsed to one line so a multi-line body cannot break the log layout.
  const std::string one_line = text::CollapseWhitespace(message);

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