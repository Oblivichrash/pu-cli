// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "pu/llm/model_verifier.hpp"
#include "pu/core/base.hpp"
#include "tests/mocks/mock_http_client.hpp"

using namespace pu;
using namespace pu::tests;

namespace {

config::BackendConfig BackendOfType(config::BackendType type, const std::string& host,
                                    const std::string& key = "") {
  config::BackendConfig cfg;
  cfg.type = type;
  cfg.host = host;
  cfg.model = "the-active-model";
  if (!key.empty()) cfg.api_key = key;
  return cfg;
}

void AnswerWithContent(MockHttpClient& mock, const std::string& content) {
  mock.simulate_response = [content](const std::string&, const std::string&,
                                     const std::vector<std::string>&,
                                     pu::http::WriteCallback write_cb) {
    const std::string frame =
        "data: {\"choices\":[{\"delta\":{\"content\":\"" + content + "\"}}]}\n\n" +
        std::string("data: [DONE]\n\n");
    write_cb(const_cast<char*>(frame.data()), frame.size());
  };
}

}  // namespace

TEST_CASE("[model_verifier] a model that answers is reported as answered",
          "[model_verifier]") {
  auto [http, mock] = MakeMockHttpClient();
  AnswerWithContent(*mock, "p");

  const auto checks = llm::VerifyModels(
      BackendOfType(config::BackendType::kOpenAI, "https://x.example/v1"),
      {"m-one", "m-two"}, http);

  REQUIRE(checks.size() == 2);
  REQUIRE(checks[0].model == "m-one");
  REQUIRE(checks[0].answered);
  REQUIRE(checks[0].detail.empty());
  REQUIRE(checks[1].model == "m-two");
  REQUIRE(checks[1].answered);
}

TEST_CASE("[model_verifier] the probe names the model under test, not the active one",
          "[model_verifier]") {
  auto [http, mock] = MakeMockHttpClient();
  AnswerWithContent(*mock, "p");

  (void)llm::VerifyModels(
      BackendOfType(config::BackendType::kOpenAI, "https://x.example/v1"), {"org/two"}, http);

  REQUIRE(mock->last_body.find("\"model\":\"org/two\"") != std::string::npos);
}

TEST_CASE("[model_verifier] a refusal is a result, not a throw, and the loop goes on",
          "[model_verifier]") {
  auto [http, mock] = MakeMockHttpClient();
  int calls = 0;
  mock->simulate_response = [&calls](const std::string&, const std::string&,
                                     const std::vector<std::string>&,
                                     pu::http::WriteCallback write_cb) {
    ++calls;
    if (calls == 1) throw pu::HttpError("HTTP error 400: Model unavailable");
    const std::string frame = "data: {\"choices\":[{\"delta\":{\"content\":\"p\"}}]}\n\n";
    write_cb(const_cast<char*>(frame.data()), frame.size());
  };

  const auto checks = llm::VerifyModels(
      BackendOfType(config::BackendType::kOpenAI, "https://x.example/v1"),
      {"dead", "live"}, http);

  REQUIRE(checks.size() == 2);
  REQUIRE_FALSE(checks[0].answered);
  REQUIRE(checks[0].detail.find("Model unavailable") != std::string::npos);
  REQUIRE(checks[1].answered);
}

TEST_CASE("[model_verifier] one token is asked for, and it rides the probe",
          "[model_verifier]") {
  auto [http, mock] = MakeMockHttpClient();
  AnswerWithContent(*mock, "p");

  (void)llm::VerifyModels(
      BackendOfType(config::BackendType::kOpenAI, "https://x.example/v1"), {"m"}, http);

  REQUIRE(mock->last_body.find("\"max_tokens\":1") != std::string::npos);
}
