// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "pu/error.hpp"
#include "mocks/mock_http_client.hpp"
#include "src/config_tools/model_scanner.hpp"
#include "test_helpers.hpp"

namespace {

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    old_ = std::getenv(name) ? std::getenv(name) : "";
    had_old_ = std::getenv(name) != nullptr;
    if (value) {
      test_helpers::set_env(name, value, 1);
    } else {
      test_helpers::unset_env(name);
    }
  }
  ~ScopedEnv() {
    if (had_old_) {
      test_helpers::set_env(name_.c_str(), old_.c_str(), 1);
    } else {
      test_helpers::unset_env(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::string old_;
  bool had_old_ = false;
};

pu::config_tools::ProviderConfig OpenAiConfig(const std::string& env_var) {
  return {"test-openai", "openai_compatible", "https://api.example.com/v1",
          {env_var}, "/models", "data[].id"};
}

}  // unnamed namespace

TEST_CASE("scanProvider omits auth header when env var is unset",
          "[model_scanner]") {
  ScopedEnv env("TEST_API_KEY", nullptr);
  pu::tests::MockHttpClient mock;
  mock.get_response = R"({"data":[]})";

  auto models = pu::config_tools::scanProvider(OpenAiConfig("TEST_API_KEY"), mock);

  REQUIRE(mock.last_get_url == "https://api.example.com/v1/models");
  REQUIRE(mock.last_get_headers.empty());
  REQUIRE(models.empty());
}

TEST_CASE("scanProvider omits auth header when config has no env var",
          "[model_scanner]") {
  pu::tests::MockHttpClient mock;
  mock.get_response = R"({"data":[]})";

  auto models = pu::config_tools::scanProvider(OpenAiConfig(""), mock);

  REQUIRE(mock.last_get_headers.empty());
  REQUIRE(models.empty());
}

TEST_CASE("scanProvider propagates HTTP errors", "[model_scanner]") {
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&, const std::vector<std::string>&) -> std::string {
    throw pu::HttpError("HTTP error 401: Unauthorized");
  };

  REQUIRE_THROWS_AS(pu::config_tools::scanProvider(OpenAiConfig(""), mock),
                    pu::HttpError);
}

TEST_CASE("scanProvider returns empty vector for malformed JSON",
          "[model_scanner]") {
  pu::tests::MockHttpClient mock;
  mock.get_response = "not json";
  REQUIRE(pu::config_tools::scanProvider(OpenAiConfig(""), mock).empty());
}
