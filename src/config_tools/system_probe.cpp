// SPDX-License-Identifier: GPL-3.0-only
#include "system_probe.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

#include "pu/infra/platform.hpp"
#include "infra/curl_http_client.hpp"
#include "config_tools/provider_registry.hpp"

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

bool HasEnv(const std::string& name) {
  const char* val = std::getenv(name.c_str());
  return val && *val;
}

bool Reachable(pu::http::HttpClient& http, const std::string& url) {
  try {
    http.Get(url);
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

  // Status is derived from the registry so custom providers are probed too.
  nlohmann::json status;
  for (const auto& p : LoadProviders()) {
    if (p.type == "ollama") {
      // Ping the base URL: a running Ollama answers on it, unlike a check
      // that depends on an endpoint path that custom providers may not use.
      status[p.name]["is_running"] = Reachable(http, p.base_url);
    } else {
      status[p.name]["has_api_key"] = HasEnv(p.auth.env_var);
    }
  }
  j["provider_status"] = status;

  return j;
}

}  // namespace pu::config_tools
