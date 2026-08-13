// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mocks/mock_http_client.hpp"
#include "src/config_tools/model_scanner.hpp"
#include "src/config_tools/provider_registry.hpp"

namespace {

// Redirects HOME to an isolated temp dir so ~/.pu/providers.json resolution is
// deterministic regardless of the developer machine.
class ScopedHome {
 public:
  ScopedHome() : dir_(std::filesystem::temp_directory_path() /
                      ("pu_providers_" + std::to_string(counter_++))) {
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    old_ = std::getenv("HOME") ? std::getenv("HOME") : "";
    had_old_ = std::getenv("HOME") != nullptr;
    setenv("HOME", dir_.c_str(), 1);
  }
  ~ScopedHome() {
    std::filesystem::remove_all(dir_);
    if (had_old_) {
      setenv("HOME", old_.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
  }

  std::filesystem::path WriteUserProviders(const std::string& contents) {
    auto file = dir_ / ".pu" / "providers.json";
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    out << contents;
    return file;
  }

 private:
  static int counter_;
  std::filesystem::path dir_;
  std::string old_;
  bool had_old_ = false;
};

int ScopedHome::counter_ = 0;

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

TEST_CASE("LoadProviders auto-creates default providers when no file exists",
          "[provider_registry]") {
  ScopedHome home;

  auto providers = pu::config_tools::LoadProviders();

  REQUIRE(providers.size() == 3);
  REQUIRE(providers[0].name == "nim");
  REQUIRE(providers[0].type == "openai_compatible");
  REQUIRE(providers[0].base_url == "https://integrate.api.nvidia.com/v1");
  REQUIRE(providers[0].auth.env_var == "NVIDIA_API_KEY");
  REQUIRE(providers[0].model_endpoint == "/models");
  REQUIRE(providers[0].model_list_key == "data[].id");
  REQUIRE(providers[1].name == "ollama");
  REQUIRE(providers[1].type == "ollama");
  REQUIRE(providers[1].base_url == "http://localhost:11434");
  REQUIRE(providers[1].auth.env_var.empty());
  REQUIRE(providers[1].model_endpoint == "/api/tags");
  REQUIRE(providers[2].name == "openai");
  REQUIRE(providers[2].auth.env_var == "OPENAI_API_KEY");
}

TEST_CASE("LoadProviders parses a valid providers.json", "[provider_registry]") {
  ScopedHome home;
  home.WriteUserProviders(R"([
    {
      "name": "local-llama",
      "type": "openai_compatible",
      "base_url": "http://localhost:8000/v1",
      "auth": {"env_var": "LOCAL_LLAMA_KEY"},
      "model_endpoint": "/models",
      "model_list_key": "data[].id"
    },
    {
      "name": "local-ollama",
      "type": "ollama",
      "base_url": "http://localhost:11434",
      "model_endpoint": "/api/tags",
      "model_list_key": "models[].name"
    }
  ])");

  auto providers = pu::config_tools::LoadProviders();

  REQUIRE(providers.size() == 2);
  REQUIRE(providers[0].name == "local-llama");
  REQUIRE(providers[0].type == "openai_compatible");
  REQUIRE(providers[0].base_url == "http://localhost:8000/v1");
  REQUIRE(providers[0].auth.env_var == "LOCAL_LLAMA_KEY");
  REQUIRE(providers[0].model_list_key == "data[].id");
  REQUIRE(providers[1].name == "local-ollama");
  REQUIRE(providers[1].type == "ollama");
  REQUIRE(providers[1].base_url == "http://localhost:11434");
  REQUIRE(providers[1].auth.env_var.empty());
  REQUIRE(providers[1].model_list_key == "models[].name");
}

TEST_CASE("LoadProviders accepts object-wrapped provider arrays",
          "[provider_registry]") {
  ScopedHome home;
  home.WriteUserProviders(
      R"({"providers":[{"name":"wrapped","type":"openai_compatible",
                        "base_url":"https://example.com/v1",
                        "model_endpoint":"/models",
                        "model_list_key":"data[].id"}]})");

  auto providers = pu::config_tools::LoadProviders();

  REQUIRE(providers.size() == 1);
  REQUIRE(providers[0].name == "wrapped");
}

TEST_CASE("scanProvider parses openai_compatible via model_list_key",
          "[provider_registry]") {
  ScopedEnv env("LOCAL_LLAMA_KEY", "secret");
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&,
                                  const std::vector<std::string>&) {
    return R"({"data":[{"id":"llama-3.3-70b"},{"id":"deepseek-r1"}]})";
  };

  pu::config_tools::ProviderConfig cfg{
      "local-llama", "openai_compatible", "http://localhost:8000/v1",
      {"LOCAL_LLAMA_KEY"}, "/models", "data[].id"};

  auto models = pu::config_tools::scanProvider(cfg, mock);

  REQUIRE(mock.last_get_url == "http://localhost:8000/v1/models");
  REQUIRE(mock.last_get_headers ==
          std::vector<std::string>{"Authorization: Bearer secret"});
  REQUIRE(models == std::vector<std::string>{"llama-3.3-70b", "deepseek-r1"});
}

TEST_CASE("scanProvider parses ollama via models[].name",
          "[provider_registry]") {
  pu::tests::MockHttpClient mock;
  mock.simulate_get_response = [](const std::string&,
                                  const std::vector<std::string>&) {
    return R"({"models":[{"name":"qwen3.5:2b"},{"name":"llama3.2:latest"}]})";
  };

  pu::config_tools::ProviderConfig cfg{
      "local-ollama", "ollama", "http://localhost:11434", {}, "/api/tags",
      "models[].name"};

  auto models = pu::config_tools::scanProvider(cfg, mock);

  REQUIRE(mock.last_get_url == "http://localhost:11434/api/tags");
  REQUIRE(mock.last_get_headers.empty());
  REQUIRE(models == std::vector<std::string>{"qwen3.5:2b", "llama3.2:latest"});
}
