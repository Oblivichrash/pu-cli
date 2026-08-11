// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace pu::tools {

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

}  // namespace pu::tools
