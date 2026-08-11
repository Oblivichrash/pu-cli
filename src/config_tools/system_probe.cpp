// SPDX-License-Identifier: GPL-3.0-only
#include "system_probe.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "pu/infra/platform.hpp"
#include "infra/curl_http_client.hpp"

namespace pu::config_tools {

namespace {

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

std::string UnameOutput(const char* flag) {
#ifdef _WIN32
  return "";
#else
  std::string out;
  pu::platform::ExecuteCommand(std::string("uname ") + flag, out);
  return Trim(out);
#endif
}

bool HasEnv(const char* name) {
  const char* val = std::getenv(name);
  return val && *val;
}

bool OllamaRunning(pu::http::HttpClient& http) {
  try {
    http.Get("http://localhost:11434/api/tags");
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

nlohmann::json ProbeSystem() {
  pu::http::CurlHttpClient http;
  return ProbeSystem(http);
}

nlohmann::json ProbeSystem(pu::http::HttpClient& http) {
  nlohmann::json j;
#ifdef _WIN32
  j["os_name"] = "Windows";
  j["kernel_version"] = "unknown";
  j["arch"] = sizeof(void*) == 8 ? "x86_64" : "x86";
#else
  j["os_name"] = UnameOutput("-s");
  j["kernel_version"] = UnameOutput("-r");
  j["arch"] = UnameOutput("-m");
#endif
  j["working_dir"] = std::filesystem::current_path().string();

  nlohmann::json status;
  status["nim"]["has_api_key"] = HasEnv("NVIDIA_API_KEY");
  status["ollama"]["is_running"] = OllamaRunning(http);
  status["openai"]["has_api_key"] = HasEnv("OPENAI_API_KEY");
  j["provider_status"] = status;

  return j;
}

}  // namespace pu::config_tools
