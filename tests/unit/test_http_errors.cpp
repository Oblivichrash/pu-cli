// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/core/base.hpp"
#include "pu/core/beast_http_client.hpp"
#include "tests/mocks/serve_harness.hpp"

#include <cstddef>
#include <string>
#include <type_traits>

namespace {

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

TEST_CASE("A gateway that writes a user-facing message has it shown", "[http][errors]") {
  const std::string body =
      R"({"code":11102,)"
      R"("msg":"model [deepseek-v3] service info not found",)"
      R"("requestId":"req-7",)"
      R"("displayMsg":{"en":"Model unavailable. Please switch models",)"
      R"("zh":"\u5f53\u524d\u6a21\u578b\u4e0d\u53ef\u7528\uff0c\u8bf7\u5207\u6362\u6a21\u578b"},)"
      R"("actions":["SWITCH_MODEL"]})";
  pu::tests::FakeHttpServer server(400, body);
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.Port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    const std::string message = e.what();
    REQUIRE(message.find("HTTP error 400") != std::string::npos);
    REQUIRE(message.find("Model unavailable. Please switch models") != std::string::npos);
  }
}

TEST_CASE("The cause a gateway names is not what a user is shown", "[http][errors]") {
  const std::string body =
      R"({"code":11102,"msg":"model [deepseek-v3] service info not found",)"
      R"("requestId":"req-7","displayMsg":{"en":"Model unavailable. Please switch models"},)"
      R"("actions":["SWITCH_MODEL"]})";
  pu::tests::FakeHttpServer server(400, body);
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.Port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    REQUIRE(std::string(e.what()).find("service info not found") == std::string::npos);
  }
}

TEST_CASE("A gateway body naming no user message falls back to the cause", "[http][errors]") {
  const std::string body =
      R"({"code":11102,"msg":"model [deepseek-v3] service info not found","requestId":"req-7"})";
  pu::tests::FakeHttpServer server(400, body);
  pu::http::BeastHttpClient client;

  std::string received;
  try {
    Post(client, server.Port(), received);
    FAIL("expected HttpError");
  } catch (const pu::HttpError& e) {
    REQUIRE(std::string(e.what()).find("service info not found") != std::string::npos);
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
  const pu::RequestRefused refused("tool calls are pending");
  REQUIRE(StatusFor(refused) == 400);
  static_assert(!std::is_base_of_v<pu::Error, pu::RequestRefused>);

  REQUIRE(StatusFor(pu::Error("unknown backend type")) == 500);
  REQUIRE(StatusFor(pu::HttpError("HTTP read error")) == 500);
  REQUIRE(StatusFor(pu::RuntimeError("no more specific answer")) == 400);
}
