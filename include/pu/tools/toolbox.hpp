// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include "pu/config/agents.hpp"
#include "pu/core/json.hpp"
#include "pu/core/text.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

struct ToolContext {
  const config::SecurityPolicy* security = nullptr;
};

class Tool {
 public:
  virtual ~Tool() = default;

  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;
  virtual boost::json::value ParametersSchema() const = 0;
  virtual std::string Execute(const boost::json::value& args, ToolContext& ctx) = 0;

  const std::string& DisplayName() const { return display_name_; }

 private:
  friend class Toolbox;
  std::string display_name_;
};

class Toolbox {
 public:
  void RegisterTool(std::unique_ptr<Tool> tool);
  std::vector<ToolDefinition> GetToolDefinitions() const;
  std::string ExecuteTool(const std::string& name, const boost::json::value& args,
                          ToolContext& ctx);

 private:
  static std::string SanitizeToolName(const std::string& name);

  std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

namespace tools {

struct ToolResult {
  bool valid = false;
  bool success = false;
  std::string stdout_content;
  std::string stderr_content;
  std::string error;
  int exit_code = 0;
};

inline std::string MakeToolResultJson(bool success, const std::string& stdout_content,
                                      const std::string& stderr_content, const std::string& error,
                                      int exit_code) {
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
    spdlog::debug("Tool result is not structured JSON: {}", e.what());
  }
  return r;
}

}  // namespace tools

}  // namespace pu
