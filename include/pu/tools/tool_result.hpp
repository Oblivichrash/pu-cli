// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <exception>
#include <string>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include "pu/core/json.hpp"
#include "pu/core/text.hpp"

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
  // JSON strings must be UTF-8; tool output may not be.
  boost::json::value j = {
    {"success", success},
    {"stdout", text::SanitizeUtf8(stdout_content)},
    {"stderr", text::SanitizeUtf8(stderr_content)},
    {"error", text::SanitizeUtf8(error)},
    {"exit_code", exit_code},
  };
  return boost::json::serialize(j);
}

inline ToolResult ParseToolResult(const std::string& raw) {
  ToolResult r;
  try {
    auto j = boost::json::parse(raw);
    if (j.is_object() && j.as_object().contains("success")) {
      r.valid = true;
      r.success = boost::json::value_to<bool>(j.at("success"));
      r.stdout_content = json::ValueOrDefault<std::string>(j, "stdout", "");
      r.stderr_content = json::ValueOrDefault<std::string>(j, "stderr", "");
      r.error = json::ValueOrDefault<std::string>(j, "error", "");
      r.exit_code = json::ValueOrDefault<int>(j, "exit_code", 0);
    }
  } catch (const std::exception& e) {
    // Raw built-in/MCP text is valid tool output; keep it, but log so
    // malformed structured output stays diagnosable.
    spdlog::debug("Tool result is not structured JSON: {}", e.what());
  }
  return r;
}

inline std::string ExtractToolResultContent(const std::string& tool_result) {
  auto r = ParseToolResult(tool_result);
  if (!r.valid) return tool_result;
  return r.success ? r.stdout_content : r.error;
}

}  // namespace pu::tools
