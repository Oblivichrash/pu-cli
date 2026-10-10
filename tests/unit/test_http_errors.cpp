// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/core/base.hpp"
#include "pu/core/beast_http_client.hpp"
#include "tests/mocks/serve_harness.hpp"

#include <cstddef>
#include <string>

namespace {

// What the layers below threw, as a status. Kept beside the mapping it checks rather than
// exercised only through a route, because a route reads the type the same way.
unsigned StatusFor(const std::exception& e) {
  if (dynamic_cast<const pu::Error*>(&e) != nullptr) return 500;
  if (dynamic_cast<const pu::RuntimeError*>(&e) != nullptr) return 400;
  return 500;
}

std::string Post(pu::http::BeastHttpClient& client, int port, std::string& received) {
  const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
  client.PostStream(url, "{}", {"Content-Type: application/json"},
                    [&received](char* data, size_t size) {
                      received.append(data, size);
                      return size;
                    });
  return received;
}

}  // namespace

TEST_CASE("A failure response reports what the server said", "[http][errors]") {
  const std::string body =
      R"({"error":{"message":"maximum context length is 4096 tokens","type":"invalid_request_error"}})";
  pu::tests::FakeHttpServer server(400, body);
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.Port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    const std::string message = e.what();
    REQUIRE(message.find("HTTP error 400") != std::string::npos);
    REQUIRE(message.find("maximum context length is 4096 tokens") != std::string::npos);
  }

  // The body is a failure message, not stream content.
  REQUIRE(received.empty());
}

TEST_CASE("A flat error string is reported too", "[http][errors]") {
  pu::tests::FakeHttpServer server(429, R"({"error":"rate limit reached"})");
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.Port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    REQUIRE(std::string(e.what()).find("rate limit reached") != std::string::npos);
  }
}

TEST_CASE("A non-JSON failure body is reported verbatim", "[http][errors]") {
  pu::tests::FakeHttpServer server(503, "upstream is down");
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.Port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    REQUIRE(std::string(e.what()).find("upstream is down") != std::string::npos);
  }
}

TEST_CASE("Failure detail is one line and bounded", "[http][errors]") {
  const std::string body = "line one\nline two\tline three";
  pu::tests::FakeHttpServer server(400, body);
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.Port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    const std::string message = e.what();
    REQUIRE(message.find('\n') == std::string::npos);
    REQUIRE(message.find("line one line two line three") != std::string::npos);
  }
}

TEST_CASE("A success response still streams to the consumer", "[http][errors]") {
  pu::tests::FakeHttpServer server(200, "streamed content");
  pu::http::BeastHttpClient client;

  std::string received;
  Post(client, server.Port(), received);

  REQUIRE(received == "streamed content");
}

TEST_CASE("The error a request is refused with decides its status", "[http][errors]") {
  // A request the state cannot serve is the caller's to act on, so it is reported as 400 and
  // must not be an Error: anything derived from Error reads as a fault of this process.
  const pu::RequestRefused refused("tool calls are pending");
  REQUIRE(StatusFor(refused) == 400);
  REQUIRE(dynamic_cast<const pu::Error*>(&refused) == nullptr);

  // A request that is itself wrong, and a failure with no more specific answer, are faults.
  REQUIRE(StatusFor(pu::Error("unknown backend type")) == 500);
  REQUIRE(StatusFor(pu::HttpError("HTTP read error")) == 500);
  REQUIRE(StatusFor(pu::RuntimeError("no more specific answer")) == 400);
}
