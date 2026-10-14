// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "pu/config/backend.hpp"
#include "tests/mocks/mock_http_client.hpp"

using namespace pu;
using namespace pu::tests;

namespace {

config::BackendConfig BackendOfType(config::GatewayId type, const std::string& host,
                                    const std::string& key = "") {
  config::BackendConfig cfg;
  cfg.gateway = type;
  cfg.host = host;
  cfg.model = "the-active-model";
  if (!key.empty()) cfg.api_key = key;
  return cfg;
}

bool HasHeader(const std::vector<std::string>& headers, const std::string& name) {
  return std::any_of(headers.begin(), headers.end(), [&name](const std::string& header) {
    return header.rfind(name + ":", 0) == 0;
  });
}

std::vector<std::string> IdsOf(const std::vector<llm::ModelEntry>& entries) {
  std::vector<std::string> ids;
  ids.reserve(entries.size());
  for (const llm::ModelEntry& entry : entries) ids.push_back(entry.id);
  return ids;
}

}  // namespace

TEST_CASE("[list_models] ollama reads /api/tags and the models' names", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"models":[{"name":"gpt-oss:20b"},{"name":"q:2b"}]})"};
  };

  const auto models = config::ListModels(
      BackendOfType(config::GatewayId::kOllama, "http://localhost:11434"), *mock);
  REQUIRE(IdsOf(models) == std::vector<std::string>{"gpt-oss:20b", "q:2b"});
  REQUIRE(mock->last_get_url == "http://localhost:11434/api/tags");
}

TEST_CASE("[list_models] openai reads /models and the data array's ids", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[{"id":"org/one"},{"id":"org/two"}]})"};
  };

  const auto models = config::ListModels(
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"), *mock);
  REQUIRE(IdsOf(models) == std::vector<std::string>{"org/one", "org/two"});
  REQUIRE(mock->last_get_url == "https://x.example/v1/models");
}

TEST_CASE("[list_models] an api key rides the request as a bearer header", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[]})"};
  };

  (void)config::ListModels(
      BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1", "nvapi-secret"),
      *mock);
  REQUIRE(mock->last_headers.size() == 1);
  REQUIRE(mock->last_headers[0] == "Authorization: Bearer nvapi-secret");
}

TEST_CASE("[list_models] a listing that names no model is empty, not an error",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"models": null})"};
  };

  const auto models = config::ListModels(
      BackendOfType(config::GatewayId::kOllama, "http://localhost:11434"), *mock);
  REQUIRE(models.empty());
}

TEST_CASE("[list_models] a body that does not parse is an Error, not a crash",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, "404 page not found"};
  };

  REQUIRE_THROWS_AS(config::ListModels(
                        BackendOfType(config::GatewayId::kOpenAI, "https://x.example/v1"),
                        *mock),
                    pu::Error);
}

TEST_CASE("[list_models] codebuddy reads /v3/config and each agent's models",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  const std::string body =
      "{\"code\":0,\"data\":{"
      "\"agents\":[{\"name\":\"cli\",\"models\":[\"glm-5.2\",\"hy3\"]}],"
      "\"models\":[{\"id\":\"glm-5.2\"},{\"id\":\"hunyuan-image-alpha\"}]}}";
  mock->simulate_get = [body](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, body};
  };

  const std::string host = config::DefaultHostFor(config::GatewayId::kCodeBuddy);
  const auto models =
      config::ListModels(BackendOfType(config::GatewayId::kCodeBuddy, host), *mock);

  REQUIRE(mock->last_get_url == "https://copilot.tencent.com/v3/config");
  REQUIRE(IdsOf(models) == std::vector<std::string>{"glm-5.2", "hy3"});
  REQUIRE(HasHeader(mock->last_headers, "User-Agent"));
  REQUIRE(HasHeader(mock->last_headers, "X-User-Id"));
}

TEST_CASE("[list_models] a codebuddy key rides the config read too", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  const std::string body = "{\"data\":{\"agents\":[{\"models\":[]}]}}";
  mock->simulate_get = [body](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, body};
  };

  (void)config::ListModels(
      BackendOfType(config::GatewayId::kCodeBuddy, "https://copilot.tencent.com/v2", "ck_x"),
      *mock);
  REQUIRE(HasHeader(mock->last_headers, "Authorization"));
}
