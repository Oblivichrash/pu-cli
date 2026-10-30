// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "tests/mocks/mock_http_client.hpp"
#include "pu/llm/llm_provider.hpp"

#include <boost/json.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

// A provider test does the same four things every time: fill a config, make a
// mock, hand the mock to the provider, and send one turn so the request it built
// can be read back. Only the config differs from one test to the next, so the
// harness takes the config and owns the rest.
//
// `Build` leaves the request in the mock without sending a turn, for the tests
// that assert on what asking the provider to construct would have produced.
// `Send` also sends one turn from a single user message. `Harness::Body` parses
// the request the mock captured.

namespace pu::tests {

// Read a captured response as the bytes a gateway actually sent, rather than a
// rewrite of them.
inline std::string ReadFixture(const std::string& name) {
  std::ifstream file(std::string(PU_FIXTURES_DIR) + "/" + name, std::ios::binary);
  REQUIRE(file.is_open());
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

template <typename Provider, typename Config>
class ProviderHarness {
 public:
  explicit ProviderHarness(Config config) {
    auto [mock, captured] = MakeMockHttpClient();
    mock_ = std::move(mock);
    captured_ = captured;
    provider_ = std::make_unique<Provider>(std::move(config), std::move(mock_));
  }

  Provider& operator*() { return *provider_; }
  Provider* operator->() { return provider_.get(); }

  MockHttpClient& Mock() { return *captured_; }

  // A one-message history, for the tests that send a turn they build themselves.
  static std::vector<ChatMessage> History(std::string user_message = "Hi") {
    return {{1, "now", "user", std::move(user_message)}};
  }

  // The request the provider built, parsed. Read it after a turn has been sent
  // (or after `Build`, which is what a turn would send).
  boost::json::value Body() const { return boost::json::parse(captured_->last_body); }

  // One user message, one turn, and the reply.
  ChatResult Send(std::string user_message = "Hi") {
    std::vector<ChatMessage> history = {{1, "now", "user", std::move(user_message)}};
    return provider_->Chat(ChatRequest(history, {}));
  }

  // One user message, one turn, and the body that turn sent.
  boost::json::value BodyOf(std::string user_message = "Hi") {
    Send(std::move(user_message));
    return Body();
  }

 private:
  std::shared_ptr<MockHttpClient> mock_;
  MockHttpClient* captured_ = nullptr;
  std::unique_ptr<Provider> provider_;
};

}  // namespace pu::tests
