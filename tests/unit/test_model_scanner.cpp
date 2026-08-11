// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "pu/error.hpp"
#include "mocks/mock_http_client.hpp"
#include "src/config_tools/model_scanner.hpp"

namespace {

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    old_ = std::getenv(name) ? std::getenv(name) : "";
    had_old_ = std::getenv(name) != nullptr;
    if (value) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }
  ~ScopedEnv() {
    if (had_old_) {
      setenv(name_.c_str(), old_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::string old_;
  bool had_old_ = false;
};

}  // unnamed namespace

TEST_CASE("scanNvidiaNIM parses data[].id and sends bearer header", "[model_scanner]") {
  ScopedEnv env("NVIDIA_API_KEY", "nim-key");
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&, const std::vector<std::string>&) {
    return R"({"object":"list","data":[
      {"id":"meta/llama-3.3-70b-instruct"},
      {"id":"deepseek-ai/deepseek-r1"}
    ]})";
  };

  auto models = pu::config_tools::scanNvidiaNIM(mock);

  REQUIRE(mock.last_get_url == "https://integrate.api.nvidia.com/v1/models");
  REQUIRE(mock.last_get_headers.size() == 1);
  REQUIRE(mock.last_get_headers[0] == "Authorization: Bearer nim-key");
  REQUIRE(models == std::vector<std::string>{"meta/llama-3.3-70b-instruct",
                                             "deepseek-ai/deepseek-r1"});
}

TEST_CASE("scanNvidiaNIM omits auth header when key is unset", "[model_scanner]") {
  ScopedEnv env("NVIDIA_API_KEY", nullptr);
  pu::tests::MockHttpClient mock;
  mock.get_response = R"({"data":[]})";

  auto models = pu::config_tools::scanNvidiaNIM(mock);

  REQUIRE(mock.last_get_headers.empty());
  REQUIRE(models.empty());
}

TEST_CASE("scanOllama parses models[].name", "[model_scanner]") {
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&, const std::vector<std::string>&) {
    return R"({"models":[
      {"name":"qwen3.5:2b","size":1},
      {"name":"llama3.2:latest","size":2}
    ]})";
  };

  auto models = pu::config_tools::scanOllama(mock);

  REQUIRE(mock.last_get_url == "http://localhost:11434/api/tags");
  REQUIRE(models == std::vector<std::string>{"qwen3.5:2b", "llama3.2:latest"});
}

TEST_CASE("scanOpenAICompatible normalizes host and sends bearer header", "[model_scanner]") {
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&, const std::vector<std::string>&) {
    return R"({"data":[{"id":"gpt-4o-mini"},{"id":"gpt-4o"}]})";
  };

  auto models =
      pu::config_tools::scanOpenAICompatible("https://api.openai.com/v1/", "sk-test", mock);

  REQUIRE(mock.last_get_url == "https://api.openai.com/v1/models");
  REQUIRE(mock.last_get_headers[0] == "Authorization: Bearer sk-test");
  REQUIRE(models == std::vector<std::string>{"gpt-4o-mini", "gpt-4o"});
}

TEST_CASE("scanOpenAICompatible sends no auth header for empty key", "[model_scanner]") {
  pu::tests::MockHttpClient mock;
  mock.get_response = R"({"data":[]})";

  auto models = pu::config_tools::scanOpenAICompatible("http://localhost:8000", "", mock);

  REQUIRE(mock.last_get_headers.empty());
  REQUIRE(models.empty());
}

TEST_CASE("scanner propagates HTTP errors", "[model_scanner]") {
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&, const std::vector<std::string>&) -> std::string {
    throw pu::HttpError("HTTP error 401: Unauthorized");
  };

  REQUIRE_THROWS_AS(pu::config_tools::scanOllama(mock), pu::HttpError);
}

TEST_CASE("scanner returns empty vector for malformed JSON", "[model_scanner]") {
  pu::tests::MockHttpClient mock;
  mock.get_response = "not json";
  REQUIRE(pu::config_tools::scanOllama(mock).empty());
}
