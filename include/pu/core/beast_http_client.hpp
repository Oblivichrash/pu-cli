// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/core/http_client.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <functional>
#include <string>

namespace pu::http {

namespace beast = boost::beast;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class BeastHttpClient : public HttpClient {
 public:
  BeastHttpClient();
  ~BeastHttpClient() override = default;

  BeastHttpClient(const BeastHttpClient&) = delete;
  BeastHttpClient& operator=(const BeastHttpClient&) = delete;

  void StreamRequest(HttpMethod method, const std::string& url, const std::string& body,
                     const std::vector<std::string>& headers, WriteCallback write_cb,
                     CancelToken cancel_token = nullptr) override;

  Response Get(const std::string& url, const std::vector<std::string>& headers) override;

  void SetInterruptChecker(std::function<bool()> checker);

 private:
  struct UrlParts {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
  };

  UrlParts ParseUrl(const std::string& url) const;
  void CheckCancel(CancelToken token) const;

  // Every request builds its own `io_context`: this client is shared between the turn thread and
  // serve request threads, and `run()` on one context would have them driving each other's
  // operations. The TLS context is the piece all of them do share.
  net::ssl::context ssl_ctx_;
  std::function<bool()> interrupt_checker_;
};

}  // namespace pu::http
