// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "pu/config/backend.hpp"
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

}  // namespace

TEST_CASE("[list_models] ollama reads /api/tags and the models' names", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"models":[{"name":"gpt-oss:20b"},{"name":"q:2b"}]})"};
  };

  const auto models = config::ListModels(BackendOfType(config::BackendType::kOllama,
                                                       "http://localhost:11434"),
                                         *mock);
  REQUIRE(models == std::vector<std::string>{"gpt-oss:20b", "q:2b"});
  REQUIRE(mock->last_get_url == "http://localhost:11434/api/tags");
}

TEST_CASE("[list_models] openai reads /models and the data array's ids", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[{"id":"org/one"},{"id":"org/two"}]})"};
  };

  const auto models =
      config::ListModels(BackendOfType(config::BackendType::kOpenAI, "https://x.example/v1"),
                         *mock);
  REQUIRE(models == std::vector<std::string>{"org/one", "org/two"});
  REQUIRE(mock->last_get_url == "https://x.example/v1/models");
}

TEST_CASE("[list_models] an api key rides the request as a bearer header", "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, R"({"data":[]})"};
  };

  (void)config::ListModels(
      BackendOfType(config::BackendType::kOpenAI, "https://x.example/v1", "nvapi-secret"),
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
      BackendOfType(config::BackendType::kCodeBuddy, "https://x.example/v2"), *mock);
  REQUIRE(models.empty());
}

TEST_CASE("[list_models] a body that does not parse is an Error, not a crash",
          "[list_models]") {
  auto [http, mock] = MakeMockHttpClient();
  mock->simulate_get = [](const std::string&, const std::vector<std::string>&) {
    return pu::http::Response{200, "404 page not found"};
  };

  REQUIRE_THROWS_AS(config::ListModels(
                        BackendOfType(config::BackendType::kOpenAI, "https://x.example/v1"),
                        *mock),
                    pu::Error);
}
