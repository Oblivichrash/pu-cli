// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "pu/error.hpp"
#include "mocks/mock_http_client.hpp"
#include "src/config_tools/system_probe.hpp"
#include "test_helpers.hpp"

namespace {

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    old_ = std::getenv(name) ? std::getenv(name) : "";
    had_old_ = std::getenv(name) != nullptr;
    if (value) {
      test_helpers::set_env(name, value);
    } else {
      test_helpers::unset_env(name);
    }
  }
  ~ScopedEnv() {
    if (had_old_) {
      test_helpers::set_env(name_.c_str(), old_.c_str());
    } else {
      test_helpers::unset_env(name_.c_str());
    }
  }

 private:
  std::string name_;
  std::string old_;
  bool had_old_ = false;
};

// Redirects HOME to an empty temp dir so provider_status reflects the built-in
// provider registry no matter what exists on the developer machine.
class ScopedHome {
 public:
  ScopedHome() : dir_(std::filesystem::temp_directory_path() /
                      ("pu_probe_" + std::to_string(counter_++))) {
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    old_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    had_old_ = std::getenv("HOME") != nullptr;
    test_helpers::set_env("HOME", dir_.string().c_str());
  }
  ~ScopedHome() {
    std::filesystem::remove_all(dir_);
    if (had_old_) {
      test_helpers::set_env("HOME", old_.c_str());
    } else {
      test_helpers::unset_env("HOME");
    }
  }

 private:
  static int counter_;
  std::filesystem::path dir_;
  std::string old_;
  bool had_old_ = false;
};

int ScopedHome::counter_ = 0;

}  // unnamed namespace

TEST_CASE("ProbeSystem reports API key env vars", "[system_probe]") {
  ScopedHome home;
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
  ScopedHome home;
  ScopedEnv nim("NVIDIA_API_KEY", nullptr);
  ScopedEnv openai("OPENAI_API_KEY", nullptr);
  pu::tests::MockHttpClient mock;
  mock.get_response = R"({"models":[]})";

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j["provider_status"]["nim"]["has_api_key"] == false);
  REQUIRE(j["provider_status"]["openai"]["has_api_key"] == false);
}

TEST_CASE("ProbeSystem detects Ollama running by pinging base_url",
          "[system_probe]") {
  ScopedHome home;
  pu::tests::MockHttpClient mock;
  mock.get_response = "Ollama is running";

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j["provider_status"]["ollama"]["is_running"] == true);
  REQUIRE(mock.last_get_url == "http://localhost:11434");
}

TEST_CASE("ProbeSystem detects Ollama down", "[system_probe]") {
  ScopedHome home;
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&, const std::vector<std::string>&) -> std::string {
    throw pu::HttpError("connection refused");
  };

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j["provider_status"]["ollama"]["is_running"] == false);
}

TEST_CASE("ProbeSystem returns valid JSON structure", "[system_probe]") {
  ScopedHome home;
  pu::tests::MockHttpClient mock;
  mock.get_response = "{}";

  auto j = pu::config_tools::ProbeSystem(mock);

  REQUIRE(j.is_object());
  REQUIRE(j["provider_status"].is_object());
  REQUIRE(j["working_dir"].is_string());
  REQUIRE(!j["working_dir"].get<std::string>().empty());
}
