// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace pu::tools {

struct ToolResult {
  bool valid = false;
  bool success = false;
  std::string stdout_content;
  std::string stderr_content;
  std::string error;
  int exit_code = 0;
};

inline std::string MakeToolResultJson(bool success,
                                      const std::string& stdout_content,
                                      const std::string& stderr_content,
                                      const std::string& error,
                                      int exit_code) {
  nlohmann::json j;
  j["success"] = success;
  j["stdout"] = stdout_content;
  j["stderr"] = stderr_content;
  j["error"] = error;
  j["exit_code"] = exit_code;
  return j.dump();
}

inline ToolResult ParseToolResult(const std::string& raw) {
  ToolResult r;
  try {
    auto j = nlohmann::json::parse(raw);
    if (j.is_object() && j.contains("success")) {
      r.valid = true;
      r.success = j["success"].get<bool>();
      r.stdout_content = j.value("stdout", std::string{});
      r.stderr_content = j.value("stderr", std::string{});
      r.error = j.value("error", std::string{});
      r.exit_code = j.value("exit_code", 0);
    }
  } catch (...) {
  }
  return r;
}

inline std::string ExtractToolResultContent(const std::string& tool_result) {
  auto r = ParseToolResult(tool_result);
  if (!r.valid) return tool_result;
  return r.success ? r.stdout_content : r.error;
}

}  // namespace pu::tools
