// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <vector>

#include "pu/llm/model_listing.hpp"
#include "pu/llm/llm_provider.hpp"
#include "pu/config/backend.hpp"
#include "pu/core/base.hpp"
#include "tests/mocks/mock_http_client.hpp"
#include "tests/mocks/test_helpers.hpp"

using namespace pu;
using namespace pu::tests;

namespace {

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

// A catalogue entry that carries no capability, which is what a listing naming only an id
// reports.
std::vector<llm::ModelEntry> EntriesOf(std::initializer_list<const char*> ids) {
  std::vector<llm::ModelEntry> entries;
  for (const char* id : ids) entries.push_back(llm::ModelEntry{id, {}});
  return entries;
}

}  // namespace

TEST_CASE("[model_verifier] a model that answers is reported as answered",
          "[model_verifier]") {
  auto [http, mock] = MakeMockHttpClient();
  AnswerWithContent(*mock, "p");

  const auto checks = llm::VerifyModels(
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"),
      EntriesOf({"m-one", "m-two"}), http);

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
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), EntriesOf({"org/two"}),
      http);

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
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"),
      EntriesOf({"dead", "live"}), http);

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
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), EntriesOf({"m"}), http);

  REQUIRE(mock->last_body.find("\"max_tokens\":1") != std::string::npos);
}

TEST_CASE("[model_verifier] a model the catalogue needs a level for is not asked without one",
          "[model_verifier]") {
  auto [http, mock] = MakeMockHttpClient();
  int calls = 0;
  mock->simulate_response = [&calls](const std::string&, const std::string&,
                                     const std::vector<std::string>&,
                                     pu::http::WriteCallback write_cb) {
    ++calls;
    const std::string frame = "data: {\"choices\":[{\"delta\":{\"content\":\"p\"}}]}\n\n";
    write_cb(const_cast<char*>(frame.data()), frame.size());
  };

  llm::ModelEntry needs_level{"glm-5.1", {}};
  needs_level.caps.only_reasoning = true;
  needs_level.caps.default_effort = "medium";

  // Both values a backend can hold that put no level on the wire: unset, and disabled.
  for (const ThinkingLevel level : {ThinkingLevel::kServerDefault, ThinkingLevel::kNone}) {
    calls = 0;
    config::Backend cfg =
        BackendOfType(config::GatewayId::kCodeBuddy, "https://copilot.tencent.com/v2");
    cfg.thinking = level;

    const auto checks = llm::VerifyModels(cfg, {needs_level, llm::ModelEntry{"m-one", {}}}, http);

    REQUIRE(checks.size() == 2);
    REQUIRE(checks[0].model == "glm-5.1");
    REQUIRE_FALSE(checks[0].answered);
    REQUIRE(checks[0].detail.find("reasoning level") != std::string::npos);
    REQUIRE(checks[0].detail.find("medium") != std::string::npos);
    REQUIRE(checks[1].answered);
    REQUIRE(calls == 1);
  }
}

TEST_CASE("[model_verifier] a level the backend sets is what the probe carries",
          "[model_verifier]") {
  auto [http, mock] = MakeMockHttpClient();
  AnswerWithContent(*mock, "p");

  config::Backend cfg =
      BackendOfType(config::GatewayId::kCodeBuddy, "https://copilot.tencent.com/v2");
  cfg.thinking = ThinkingLevel::kMedium;

  llm::ModelEntry needs_level{"glm-5.1", {}};
  needs_level.caps.only_reasoning = true;

  const auto checks = llm::VerifyModels(cfg, {needs_level}, http);

  REQUIRE(checks[0].answered);
  REQUIRE(mock->last_body.find("\"reasoning_effort\":\"medium\"") != std::string::npos);
}

TEST_CASE("[model_verifier] the listing is rendered the one way, whichever command asked",
          "[model_verifier]") {
  const std::vector<llm::ModelEntry> models = EntriesOf({"alpha", "the-active-model", "beta"});

  const std::string rendered =
      llm::FormatModelList(models, "openai", "https://example.invalid/v1", "the-active-model");

  REQUIRE(rendered.find("3 model(s) reported by openai at https://example.invalid/v1") !=
          std::string::npos);
  REQUIRE(rendered.find("the-active-model (active)") != std::string::npos);
  REQUIRE(rendered.find("alpha (active)") == std::string::npos);
}

TEST_CASE("[model_verifier] a listing that names no model says so rather than heading nothing",
          "[model_verifier]") {
  const std::string rendered =
      llm::FormatModelList({}, "ollama", "http://127.0.0.1:1", "whatever");

  REQUIRE(rendered == "No models reported by ollama at http://127.0.0.1:1");
}
