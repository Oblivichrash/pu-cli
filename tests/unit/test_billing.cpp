// SPDX-License-Identifier: GPL-3.0-only

//
// What a product says a call cost, read from the field that product names.
//
// Two products share the OpenAI wire and only one of them prices a call. A parser that
// defaulted to zero turned "this product says nothing" into "this call was free", which is
// the difference between a total that can be trusted and one that cannot.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <memory>
#include <string>
#include <vector>

#include "pu/config/gateway.hpp"
#include "pu/llm/openai_provider.hpp"
#include "tests/mocks/mock_http_client.hpp"

namespace {

// One SSE answer whose usage object is `usage`, so a case can vary only the price field.
pu::OpenAIProvider::Config ConfigWithUsage(const std::string& usage, pu::config::Billing billing) {
  pu::OpenAIProvider::Config config;
  config.model = "test-model";
  config.billing = billing;
  return config;
}

std::vector<pu::ChatMessage> OneQuestion() { return {{1, "now", "user", "Hi"}}; }

}  // namespace

TEST_CASE("A price is read only from the field the product names", "[openai][usage][billing]") {
  auto [mock_http, mock_ptr] = pu::tests::MakeMockHttpClient();
  const std::string body =
      R"(data: {"choices":[],"usage":{"prompt_tokens":12,"completion_tokens":3,"credit":0.05}})"
      "\n"
      "data: [DONE]\n";
  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::vector<char> buffer(body.begin(), body.end());
    cb(buffer.data(), buffer.size());
  };

  // The product reports a price: read it, and keep the unit the product named.
  pu::OpenAIProvider reporting(ConfigWithUsage(
                               "{}", {pu::config::BillingKind::kReported, "credit", "credits"}),
                           std::move(mock_http));
  auto priced = reporting.Chat(pu::ChatRequest(OneQuestion(), {}));
  REQUIRE(priced.usage.has_value());
  REQUIRE(priced.usage->cost.has_value());
  REQUIRE(priced.usage->cost->amount == 0.05);
  REQUIRE(priced.usage->cost->unit == "credits");
}

TEST_CASE("A product that reports no price leaves the cost absent", "[openai][usage][billing]") {
  auto [mock_http, mock_ptr] = pu::tests::MakeMockHttpClient();
  // The same bytes: a `credit` field is present, and this product does not price calls.
  const std::string body =
      R"(data: {"choices":[],"usage":{"prompt_tokens":12,"completion_tokens":3,"credit":0.05}})"
      "\n"
      "data: [DONE]\n";
  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::vector<char> buffer(body.begin(), body.end());
    cb(buffer.data(), buffer.size());
  };

  pu::OpenAIProvider silent(ConfigWithUsage("{}", {}), std::move(mock_http));
  auto result = silent.Chat(pu::ChatRequest(OneQuestion(), {}));

  REQUIRE(result.usage.has_value());
  // Absent, not zero: this product says nothing about price, and a zero would be a number it
  // never sent — the one mistake that makes a summed total untrustworthy.
  REQUIRE_FALSE(result.usage->cost.has_value());
  // The prompt is still readable from the product that sends no price.
  REQUIRE(result.usage->prompt_tokens == 12);
}

TEST_CASE("A price that is not a number is not read as one", "[openai][usage][billing]") {
  auto [mock_http, mock_ptr] = pu::tests::MakeMockHttpClient();
  // Some products send a formatted string where a number is expected. Reading it as zero
  // would silently undercount a total.
  const std::string body =
      R"(data: {"choices":[],"usage":{"prompt_tokens":12,"credit":"0.05 credits"}})"
      "\n"
      "data: [DONE]\n";
  mock_ptr->simulate_response = [&](const std::string&, const std::string&,
                                    const std::vector<std::string>&, pu::http::WriteCallback cb) {
    std::vector<char> buffer(body.begin(), body.end());
    cb(buffer.data(), buffer.size());
  };

  pu::OpenAIProvider reporting(ConfigWithUsage(
                               "{}", {pu::config::BillingKind::kReported, "credit", "credits"}),
                           std::move(mock_http));
  auto result = reporting.Chat(pu::ChatRequest(OneQuestion(), {}));

  REQUIRE(result.usage.has_value());
  REQUIRE_FALSE(result.usage->cost.has_value());
}
