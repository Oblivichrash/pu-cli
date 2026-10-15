// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

#include "pu/config/security_policy.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/core/text.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu {

struct ToolContext {
  const config::SecurityPolicy* security = nullptr;
  // Who can stop a running command, and what the user wants the model to know
  // about it. Null when the caller offers no control, which the CLI does.
  struct Control {
    CancelToken stop_token;
    std::string note;  // the user's own words, carried in the stopped result
    std::function<void(std::string)> on_note;  // an approval's words, back to the model
  };
  const Control* control = nullptr;

  // The approval gate: a command matching the policy's approval patterns asks here
  // and parks until the answer arrives. Null where there is nobody to ask, which
  // makes every gate open - the CLI has no channel to ask on.
  struct Approval {
    // Returns the user's decision and their optional words beside it.
    std::function<bool(const std::string& command, std::string& note)> ask;
  };
  const Approval* approval = nullptr;
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

namespace tools {

struct ToolResult {
  bool valid = false;
  bool success = false;
  bool blocked = false;  // refused by policy before the command ran; error says why
  std::string output;    // everything the command itself printed, merged stream
  std::string error;     // the tool layer's verdict: why it failed or was refused
  int exit_code = 0;     // the command's own exit code, or -1 when it never ran
};

inline std::string MakeToolResultJson(bool success, const std::string& output,
                                      const std::string& error, int exit_code,
                                      bool blocked = false) {
  boost::json::value j = {
      {"success", success},
      {"blocked", blocked},
      {"output", text::SanitizeUtf8(output)},
      {"error", text::SanitizeUtf8(error)},
      {"exit_code", exit_code},
  };
  return boost::json::serialize(j);
}

// Quotes each name and joins with commas, for messages that name a whole set.
inline std::string QuoteJoin(const std::vector<std::string>& names) {
  std::string joined;
  for (const auto& name : names) {
    if (!joined.empty()) joined += ", ";
    joined += '"' + name + '"';
  }
  return joined;
}

inline ToolResult ParseToolResult(const std::string& raw) {
  ToolResult r;
  try {
    auto j = boost::json::parse(raw);
    if (j.is_object() && j.as_object().contains("success")) {
      r.valid = true;
      r.success = boost::json::value_to<bool>(j.at("success"));
      r.blocked = json::ValueOrDefault<bool>(j, "blocked", false);
      r.output = json::ValueOrDefault<std::string>(j, "output", "");
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
