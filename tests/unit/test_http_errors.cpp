// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/core/base.hpp"
#include "pu/core/beast_http_client.hpp"

#include <boost/beast.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {

// Answers every request with one fixed status and body, so the client's
// treatment of a failure response can be observed directly.
class OneResponseServer {
 public:
  OneResponseServer(unsigned status, std::string body)
      : ioc_(std::make_shared<net::io_context>()),
        acceptor_(std::make_shared<tcp::acceptor>(
            *ioc_, tcp::endpoint(net::ip::make_address("127.0.0.1"), 0))),
        status_(status),
        body_(std::move(body)) {
    port_ = acceptor_->local_endpoint().port();
    REQUIRE(port_ > 0);
    Accept();
    thread_ = std::thread([this] { ioc_->run(); });
  }

  ~OneResponseServer() {
    ioc_->stop();
    if (thread_.joinable()) thread_.join();
  }

  int port() const { return port_; }
  const std::string& consumed() const { return consumed_; }

 private:
  void Accept() {
    acceptor_->async_accept([this](boost::system::error_code ec, tcp::socket socket) {
      if (!ec) {
        std::thread([this, s = std::move(socket)]() mutable { Serve(std::move(s)); }).detach();
      }
      Accept();
    });
  }

  void Serve(tcp::socket socket) {
    try {
      beast::flat_buffer buffer;
      http::request<http::string_body> req;
      http::read(socket, buffer, req);

      http::response<http::string_body> res;
      res.result(static_cast<http::status>(status_));
      res.set(http::field::content_type, "application/json");
      res.body() = body_;
      res.prepare_payload();
      http::write(socket, res);

      beast::error_code ec;
      socket.shutdown(tcp::socket::shutdown_send, ec);
    } catch (const std::exception& e) {
      spdlog::debug("OneResponseServer: {}", e.what());
    }
  }

  std::shared_ptr<net::io_context> ioc_;
  std::shared_ptr<tcp::acceptor> acceptor_;
  int port_ = 0;
  unsigned status_;
  std::string body_;
  std::thread thread_;
  std::string consumed_;
};

std::string Post(pu::http::BeastHttpClient& client, int port,
                 std::string& received) {
  const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  client.PostStream(url, "{}", {"Content-Type: application/json"},
                    [&received](char* data, size_t size) {
                      received.append(data, size);
                      return size;
                    });
  return received;
}

}  // namespace

TEST_CASE("A failure response reports what the server said",
          "[http][errors]") {
  const std::string body = R"({"error":{"message":"maximum context length is 4096 tokens","type":"invalid_request_error"}})";
  OneResponseServer server(400, body);
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    const std::string message = e.what();
    REQUIRE(message.find("HTTP error 400") != std::string::npos);
    REQUIRE(message.find("maximum context length is 4096 tokens") != std::string::npos);
  }

  // The body is a failure message, not stream content.
  REQUIRE(received.empty());
  REQUIRE(client.GetErrorDetail().find("maximum context length") != std::string::npos);
}

TEST_CASE("A flat error string is reported too", "[http][errors]") {
  OneResponseServer server(429, R"({"error":"rate limit reached"})");
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    REQUIRE(std::string(e.what()).find("rate limit reached") != std::string::npos);
  }
}

TEST_CASE("A non-JSON failure body is reported verbatim", "[http][errors]") {
  OneResponseServer server(503, "upstream is down");
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    REQUIRE(std::string(e.what()).find("upstream is down") != std::string::npos);
  }
}

TEST_CASE("Failure detail is one line and bounded", "[http][errors]") {
  const std::string body = "line one\nline two\tline three";
  OneResponseServer server(400, body);
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    const std::string message = e.what();
    REQUIRE(message.find('\n') == std::string::npos);
    REQUIRE(message.find("line one line two line three") != std::string::npos);
  }
}

TEST_CASE("A success response still streams to the consumer",
          "[http][errors]") {
  OneResponseServer server(200, "streamed content");
  pu::http::BeastHttpClient client;

  std::string received;
  Post(client, server.port(), received);

  REQUIRE(received == "streamed content");
  REQUIRE(client.GetErrorDetail().empty());
}
