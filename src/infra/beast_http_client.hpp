// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/infra/http_client.hpp"

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
  ~BeastHttpClient() override;

  BeastHttpClient(const BeastHttpClient&) = delete;
  BeastHttpClient& operator=(const BeastHttpClient&) = delete;

  void PostStream(const std::string& url,
                  const std::string& body,
                  const std::vector<std::string>& headers,
                  WriteCallback write_cb,
                  CancelToken cancel_token = nullptr) override;

  void SetInterruptChecker(std::function<bool()> checker);
  std::string GetErrorDetail() const;

 private:
  struct UrlParts {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
  };

  UrlParts ParseUrl(const std::string& url) const;
  void CheckCancel(CancelToken token) const;

  net::io_context ioc_;
  net::ssl::context ssl_ctx_;
  std::function<bool()> interrupt_checker_;
  std::string error_detail_;
};

}  // namespace pu::http