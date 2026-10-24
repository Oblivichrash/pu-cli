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
      ERR_clear_error();
    }
    X509_free(parsed);
  }
  CertCloseStore(store, 0);

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
void AddHeaders(Request& req, const std::vector<std::string>& headers) {
  for (const auto& h : headers) {
    size_t pos = h.find(':');
    if (pos != std::string::npos) {
      req.set(text::Trim(std::string_view(h).substr(0, pos)),
              text::Trim(std::string_view(h).substr(pos + 1)));
    }
  }
}

template <typename Request>
void ApplyHeaders(Request& req, const std::string& host, const std::string& body,
                  const std::vector<std::string>& headers) {
  req.set(beast::http::field::host, host);
  req.set(beast::http::field::content_type, "application/json");
  req.set(beast::http::field::content_length, std::to_string(body.size()));
  AddHeaders(req, headers);
  req.body() = body;
}

// Deadlines for a request whose operations are asynchronous.
//
// Beast's documentation is explicit that a stream's timeout covers asynchronous operations, and
// asio's synchronous calls block where no timer can reach them. Every operation below is
// therefore started asynchronously and completed by `ioc.run()`, which is the only arrangement
// in which `expires_after` means anything. Without it, a gateway that accepts a connection and
// then says nothing held its caller for as long as the process lived.
constexpr std::chrono::seconds kConnectBudget{10};
// A turn is an idle deadline per read, not a cap on the whole reply: a model may think for a
// long time between fragments and a long answer may take many minutes.
constexpr std::chrono::seconds kStreamIdleBudget{300};
// A catalogue or health read is control-plane, not a model answering, so it gets a shorter
// leash and a single budget for the whole body.
constexpr std::chrono::seconds kReadIdleBudget{30};

// What a missed deadline is called. `timeout` is the timer firing; anything else is asio's own
// reason and is passed through unchanged.
[[noreturn]] inline void ThrowDeadline(const beast::error_code& ec, const char* what,
                                       std::chrono::seconds budget) {
  if (ec == beast::error::timeout) {
    throw HttpError(std::string("HTTP ") + what + " timed out after " +
                    std::to_string(budget.count()) + "s");
  }
  throw HttpError(std::string("HTTP ") + what + " error: " + ec.message());
}

// Starts one asynchronous operation and runs the context until it has nothing left to do.
//
// `restart()` is not optional: a context that has run out of work stays stopped, and a later
// `run()` returns immediately without performing anything. One request here is several
// operations in sequence — connect, write, read — so the second and third would silently do
// nothing and the request would "succeed" with an empty body. The handler stores its error
// rather than throwing, because an exception out of a handler leaves the context stopped
// mid-chain and the caller reads the error anyway.
template <typename Start>
beast::error_code Drive(net::io_context& ioc, Start start) {
  beast::error_code failure;
  ioc.restart();
  start([&failure](beast::error_code ec, auto&&...) { failure = ec; });
  ioc.run();
  return failure;
}

template <typename Stream>
void ConnectWithDeadline(net::io_context& ioc, Stream& stream,
                         const tcp::resolver::results_type& endpoints) {
  stream.expires_after(kConnectBudget);
  const beast::error_code ec =
      Drive(ioc, [&](auto handler) { stream.async_connect(endpoints, handler); });
  if (ec) ThrowDeadline(ec, "connect", kConnectBudget);
}

template <typename SslStream>
void HandshakeWithDeadline(net::io_context& ioc, SslStream& stream) {
  beast::get_lowest_layer(stream).expires_after(kConnectBudget);
  const beast::error_code ec = Drive(ioc, [&](auto handler) {
    stream.async_handshake(net::ssl::stream_base::client, handler);
  });
  if (ec) ThrowDeadline(ec, "TLS handshake", kConnectBudget);
}

template <typename Stream, typename Request>
void WriteWithDeadline(net::io_context& ioc, Stream& stream, const Request& req,
                       std::chrono::seconds budget) {
  beast::get_lowest_layer(stream).expires_after(budget);
  const beast::error_code ec =
      Drive(ioc, [&](auto handler) { beast::http::async_write(stream, req, handler); });
  if (ec) ThrowDeadline(ec, "write", budget);
}

// A whole body, read in one operation under one budget: what a `GET` wants.
template <typename Stream>
beast::http::response<beast::http::string_body> ReadWithDeadline(net::io_context& ioc,
                                                                Stream& stream,
                                                                beast::flat_buffer& buffer) {
  beast::http::response<beast::http::string_body> res;
  beast::get_lowest_layer(stream).expires_after(kReadIdleBudget);
  const beast::error_code ec = Drive(ioc, [&](auto handler) {
    beast::http::async_read(stream, buffer, res, handler);
  });
  if (ec) ThrowDeadline(ec, "read", kReadIdleBudget);
  return res;
}

// A streamed body, delivered fragment by fragment. A class rather than a chain of lambdas
// because the parser, the buffer and the piece being filled have to survive many asynchronous
// steps, and the deadline is re-armed before each read — an idle deadline per read is what lets
// a long answer run for minutes while a silent gateway still fails.
template <typename Stream>
class BodyStream {
 public:
  BodyStream(net::io_context& ioc, Stream& stream, beast::flat_buffer& buffer,
             WriteCallback& write_cb, std::string& error_body, CancelToken cancel_token)
      : ioc_(ioc),
        stream_(stream),
        buffer_(buffer),
        write_cb_(write_cb),
        error_body_(error_body),
        cancel_token_(cancel_token) {
    parser_.body_limit(64 * 1024 * 1024);
  }

  unsigned Read() {
    ioc_.restart();
    ReadHeader();
    ioc_.run();
    if (failure_) ThrowDeadline(failure_, "read", kStreamIdleBudget);
    if (is_error_) error_body_ = std::move(collected_);
    return status_;
  }

 private:
  void Arm() { beast::get_lowest_layer(stream_).expires_after(kStreamIdleBudget); }

  void ReadHeader() {
    Arm();
    beast::http::async_read_header(
        stream_, buffer_, parser_, [this](beast::error_code ec, std::size_t) {
          if (ec) {
            failure_ = ec;
            return;
          }
          status_ = parser_.get().result_int();
          is_error_ = status_ >= 400;
          ReadPiece();
        });
  }

  void ReadPiece() {
    if (parser_.is_done()) return;

    parser_.get().body().data = piece_;
    parser_.get().body().size = sizeof(piece_);
    Arm();
    beast::http::async_read_some(
        stream_, buffer_, parser_, [this](beast::error_code ec, std::size_t) {
          if (ec == beast::http::error::need_buffer) ec = {};
          if (ec) {
            failure_ = ec;
            return;
          }
          if (!OnPiece()) return;
          ReadPiece();
        });
  }

  // False stops the chain because the consumer refused the piece.
  bool OnPiece() {
    const std::size_t produced = sizeof(piece_) - parser_.get().body().size;
    if (produced == 0) return true;

    if (is_error_) {
      constexpr std::size_t kMaxErrorBody = 64 * 1024;
      if (collected_.size() < kMaxErrorBody) {
        collected_.append(piece_, std::min(produced, kMaxErrorBody - collected_.size()));
      }
      return true;
    }

    if (write_cb_(piece_, produced) == 0) {
      refused_ = true;
      return false;
    }
    return true;
  }

 public:
  // A consumer that took nothing is a failure of its own, named apart from a transport error.
  // It is how a cancelled turn stops reading: the provider's callback feeds the parser and then
  // checks the token, so the piece it was handed is kept before the abort is reported.
  bool Refused() const { return refused_; }

 private:
  net::io_context& ioc_;
  Stream& stream_;
  beast::flat_buffer& buffer_;
  WriteCallback& write_cb_;
  std::string& error_body_;
  CancelToken cancel_token_;
  beast::http::response_parser<beast::http::buffer_body> parser_;
  char piece_[16 * 1024];
  std::string collected_;
  beast::error_code failure_;
  unsigned status_ = 0;
  bool is_error_ = false;
  bool refused_ = false;
};

}  // namespace

namespace {

// Records the machine-readable half of a gateway rejection: the cause it names,
// its code, and the id its support would ask for. None of these belong in the
// message a user sees.
void LogErrorCause(const boost::json::value& j) {
  const std::string cause = json::ErrorMessage(j);
  const std::string request_id = json::ValueOrDefault<std::string>(j, "requestId", "");
  const std::string code = [&j] {
    const auto c = json::OptionalOrDefault<int>(j, "code");
    return c ? std::to_string(*c) : std::string{};
  }();

  if (cause.empty() && code.empty() && request_id.empty()) return;

  std::string detail;
  if (!code.empty()) detail += " code=" + code;
  if (!request_id.empty()) detail += " requestId=" + request_id;
  spdlog::warn("gateway rejected the request:{}{}", detail,
               cause.empty() ? "" : " cause=" + cause);
}

std::string SummarizeErrorBody(const std::string& body) {
  if (body.empty()) return "";

  const std::string text = platform::FromPipedOutput(body);
  std::string message;
  try {
    const boost::json::value parsed = boost::json::parse(text);
    message = json::UserFacingError(parsed);
    LogErrorCause(parsed);
  } catch (const boost::system::system_error&) {
  }
  if (message.empty()) message = text;

  const std::string one_line = text::CollapseWhitespace(message);

  constexpr std::size_t kMaxDetail = 400;
  if (one_line.size() <= kMaxDetail) return one_line;
  return one_line.substr(0, kMaxDetail) + "...";
}

}  // namespace

http::Response BeastHttpClient::Get(const std::string& url,
                                    const std::vector<std::string>& headers) {
  auto parts = ParseUrl(url);

  // A context of its own: the client is shared between the turn thread and serve request
  // threads, and two callers driving one context would run each other's operations.
  net::io_context ioc;
  tcp::resolver resolver(ioc);
  beast::tcp_stream stream(ioc);
  const auto endpoints = resolver.resolve(parts.host, parts.port);
  ConnectWithDeadline(ioc, stream, endpoints);

  beast::http::request<beast::http::string_body> req(beast::http::verb::get, parts.target, 11);
  req.set(beast::http::field::host, parts.host);
  AddHeaders(req, headers);

  beast::http::response<beast::http::string_body> res;
  beast::flat_buffer buffer;

  if (parts.scheme == "https") {
    beast::ssl_stream<beast::tcp_stream> ssl_stream(std::move(stream), ssl_ctx_);
    HandshakeWithDeadline(ioc, ssl_stream);
    WriteWithDeadline(ioc, ssl_stream, req, kReadIdleBudget);
    res = ReadWithDeadline(ioc, ssl_stream, buffer);
    beast::error_code ec;
    beast::get_lowest_layer(ssl_stream).socket().shutdown(tcp::socket::shutdown_both, ec);
  } else {
    WriteWithDeadline(ioc, stream, req, kReadIdleBudget);
    res = ReadWithDeadline(ioc, stream, buffer);
    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);
  }

  const unsigned status = res.result_int();
  if (status >= 400) {
    std::string detail = "HTTP error " + std::to_string(status);
    const std::string summary = SummarizeErrorBody(res.body());
    if (!summary.empty()) detail += ": " + summary;
    throw HttpError(detail);
  }

  return Response{status, std::move(res.body())};
}

void BeastHttpClient::StreamRequest(HttpMethod method, const std::string& url,
                                   const std::string& body,
                                   const std::vector<std::string>& headers,
                                   WriteCallback write_cb, CancelToken cancel_token) {
  const beast::http::verb verb =
      method == HttpMethod::kGet ? beast::http::verb::get : beast::http::verb::post;
  auto start = std::chrono::steady_clock::now();

  try {
    CheckCancel(cancel_token);

    auto parts = ParseUrl(url);
    const bool use_ssl = parts.scheme == "https";

    // A context of its own, for the reason `Get` has one: this client is shared.
    net::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto endpoints = resolver.resolve(parts.host, parts.port);
    CheckCancel(cancel_token);
    ConnectWithDeadline(ioc, stream, endpoints);
    CheckCancel(cancel_token);

    unsigned status = 0;
    std::string error_body;

    if (use_ssl) {
      beast::ssl_stream<beast::tcp_stream> ssl_stream(std::move(stream), ssl_ctx_);
      HandshakeWithDeadline(ioc, ssl_stream);
      CheckCancel(cancel_token);

      beast::http::request<beast::http::string_body> req(verb, parts.target, 11);
      ApplyHeaders(req, parts.host, body, headers);
      WriteWithDeadline(ioc, ssl_stream, req, kStreamIdleBudget);
      CheckCancel(cancel_token);

      beast::flat_buffer buffer;
      BodyStream<decltype(ssl_stream)> reader(ioc, ssl_stream, buffer, write_cb, error_body,
                                         cancel_token);
      status = reader.Read();
      if (reader.Refused()) throw HttpError("Streaming aborted by consumer");

      auto end = std::chrono::steady_clock::now();
      auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
      SetLogDurationMs(duration_ms);
      spdlog::trace("HTTP {} {} {}ms", url, status, duration_ms);
      ClearLogDurationMs();

      beast::error_code ec;
      beast::get_lowest_layer(ssl_stream).socket().shutdown(tcp::socket::shutdown_both, ec);
    } else {
      beast::http::request<beast::http::string_body> req(verb, parts.target, 11);
      ApplyHeaders(req, parts.host, body, headers);
      WriteWithDeadline(ioc, stream, req, kStreamIdleBudget);
      CheckCancel(cancel_token);

      beast::flat_buffer buffer;
      BodyStream<beast::tcp_stream> reader(ioc, stream, buffer, write_cb, error_body,
                                           cancel_token);
      status = reader.Read();
      if (reader.Refused()) throw HttpError("Streaming aborted by consumer");

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
