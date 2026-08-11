// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "pu/error.hpp"
#include "mocks/mock_http_client.hpp"
#include "src/config_tools/system_probe.hpp"

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

TEST_CASE("ProbeSystem reports API key env vars", "[system_probe]") {
  ScopedEnv nim("NVIDIA_API_KEY", "nim-key");
  ScopedEnv openai("OPENAI_API_KEY", "oa-key");
  pu::tests::MockHttpClient mock;
  mock.get_response = R"({"models":[]})";

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j["provider_status"]["nim"]["has_api_key"] == true);
  REQUIRE(j["provider_status"]["openai"]["has_api_key"] == true);
  REQUIRE(j.contains("os_name"));
  REQUIRE(j.contains("kernel_version"));
  REQUIRE(j.contains("arch"));
  REQUIRE(j.contains("working_dir"));
}

TEST_CASE("ProbeSystem reports missing API keys", "[system_probe]") {
  ScopedEnv nim("NVIDIA_API_KEY", nullptr);
  ScopedEnv openai("OPENAI_API_KEY", nullptr);
  pu::tests::MockHttpClient mock;
  mock.get_response = R"({"models":[]})";

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j["provider_status"]["nim"]["has_api_key"] == false);
  REQUIRE(j["provider_status"]["openai"]["has_api_key"] == false);
}

TEST_CASE("ProbeSystem detects Ollama running", "[system_probe]") {
  pu::tests::MockHttpClient mock;
  mock.get_response = R"({"models":[]})";

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j["provider_status"]["ollama"]["is_running"] == true);
  REQUIRE(mock.last_get_url == "http://localhost:11434/api/tags");
}

TEST_CASE("ProbeSystem detects Ollama down", "[system_probe]") {
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&, const std::vector<std::string>&) -> std::string {
    throw pu::HttpError("connection refused");
  };

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j["provider_status"]["ollama"]["is_running"] == false);
}

TEST_CASE("ProbeSystem returns valid JSON structure", "[system_probe]") {
  pu::tests::MockHttpClient mock;
  mock.get_response = "{}";

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j.is_object());
  REQUIRE(j["provider_status"].is_object());
  REQUIRE(j["working_dir"].is_string());
  REQUIRE(!j["working_dir"].get<std::string>().empty());
}
