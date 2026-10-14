// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "pu/llm/model_listing.hpp"
#include "tests/mocks/mock_http_client.hpp"

using namespace pu;
using namespace pu::tests;

namespace {

// The gateway's answer, trimmed to the fields a capability read names.
std::string CodeBuddyBody() {
  std::string body = "{\"code\":0,\"data\":{";
  body += "\"agents\":[{\"name\":\"cli\",\"models\":[\"glm-5.1\",\"glm-5.3\"]}],";
  body += "\"models\":[";
  body += "{\"id\":\"glm-5.1\",\"name\":\"GLM-5.1\",\"supportsToolCall\":true,";
  body += "\"supportsImages\":false,\"supportsReasoning\":true,\"onlyReasoning\":true,";
  body += "\"reasoning\":{\"effort\":\"medium\"},\"maxInputTokens\":200000},";
  body += "{\"id\":\"glm-5.3\",\"name\":\"GLM-5.3\",\"supportsToolCall\":true,";
  body += "\"supportsImages\":true,\"supportsReasoning\":true,\"onlyReasoning\":true,";
  body += "\"reasoning\":{\"effort\":\"high\"},\"maxInputTokens\":1000000}";
  body += "]}}";
  return body;
}

std::vector<llm::ModelEntry> ListCodeBuddy(const std::string& body, MockHttpClient& mock) {
  mock.simulate_get = [body](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, body};
  };
  const llm::CodeBuddyModelLister lister;
  return lister.ListModels("https://copilot.tencent.com/v2", "k", mock);
}

}  // namespace

TEST_CASE("[capabilities] a codebuddy entry carries what the host says about it",
          "[capabilities]") {
  auto [http, mock] = MakeMockHttpClient();
  const std::vector<llm::ModelEntry> models = ListCodeBuddy(CodeBuddyBody(), *mock);

  REQUIRE(models.size() == 2);
  REQUIRE(models[0].id == "glm-5.1");
  REQUIRE(models[0].caps.supports_tools);
  REQUIRE_FALSE(models[0].caps.supports_images);
  REQUIRE(models[0].caps.supports_reasoning);
  REQUIRE(models[0].caps.only_reasoning);
  REQUIRE(models[0].caps.default_effort == "medium");
  REQUIRE(models[0].caps.max_input_tokens == 200000);

  REQUIRE(models[1].id == "glm-5.3");
  REQUIRE(models[1].caps.supports_images);
  REQUIRE(models[1].caps.default_effort == "high");
  REQUIRE(models[1].caps.max_input_tokens == 1000000);
}

TEST_CASE("[capabilities] a listing naming only an id reports no capability",
          "[capabilities]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, std::string("{\"data\":[{\"id\":\"plain\"},{\"id\":\"other\"}]}")};
  };

  const llm::OpenAIModelLister lister;
  const std::vector<llm::ModelEntry> models =
      lister.ListModels("https://x.example/v1", "", *mock);

  REQUIRE(models.size() == 2);
  REQUIRE(models[0].id == "plain");
  REQUIRE_FALSE(models[0].caps.supports_tools);
  REQUIRE_FALSE(models[0].caps.only_reasoning);
  REQUIRE(models[0].caps.default_effort.empty());
  REQUIRE(models[0].caps.max_input_tokens == 0);
}

TEST_CASE("[capabilities] an ollama name is the id and carries no capability",
          "[capabilities]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, std::string("{\"models\":[{\"name\":\"gpt-oss:20b\"}]}")};
  };

  const llm::OllamaModelLister lister;
  const std::vector<llm::ModelEntry> models =
      lister.ListModels("http://localhost:11434", "", *mock);

  REQUIRE(models.size() == 1);
  REQUIRE(models[0].id == "gpt-oss:20b");
  REQUIRE_FALSE(models[0].caps.supports_reasoning);
}

TEST_CASE("[capabilities] an id an agent names but the catalogue does not still lists",
          "[capabilities]") {
  auto [http, mock] = MakeMockHttpClient();
  const std::string body = "{\"code\":0,\"data\":{\"agents\":[{\"models\":[\"orphan\"]}],\"models\":[]}}";

  const std::vector<llm::ModelEntry> models = ListCodeBuddy(body, *mock);

  REQUIRE(models.size() == 1);
  REQUIRE(models[0].id == "orphan");
  REQUIRE_FALSE(models[0].caps.only_reasoning);
}
