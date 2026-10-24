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

  // The peer this turn arrived from, empty for a turn the user started. A turn that came
  // from a peer must not reach another one, which is what keeps two servers that know each
  // other from answering each other forever.
  std::string peer_origin;
};

class Tool {
 public:
  virtual ~Tool() = default;

  virtual std::string Name() const = 0;
  virtual std::string Description() const = 0;
  virtual boost::json::value ParametersSchema() const = 0;
  virtual std::string Execute(const boost::json::value& args, ToolContext& ctx) = 0;
};

namespace tools {

// Why the policy refused a call. `kNobodyToAsk` is separate from the rest because it is not
// the command's fault: the agent asks for approval and the front end has no way to put the
// question, which is a configuration to fix rather than a command to rework.
enum class Refusal {
  kNone,
  kPolicy,      // a forbidden pattern, a dangerous command, or a user who said no
  kNobodyToAsk, // approval required, and no channel to ask on
};

struct ToolResult {
  bool valid = false;
  bool success = false;
  bool blocked = false;  // refused by policy before the command ran; error says why
  Refusal refusal = Refusal::kNone;
  std::string output;    // everything the command itself printed, merged stream
  std::string error;     // the tool layer's verdict: why it failed or was refused
  int exit_code = 0;     // the command's own exit code, or -1 when it never ran
};

inline std::string MakeToolResultJson(bool success, const std::string& output,
                                      const std::string& error, int exit_code,
                                      bool blocked = false,
                                      Refusal refusal = Refusal::kNone) {
  boost::json::value j = {
      {"success", success},
      {"blocked", blocked},
      {"output", text::SanitizeUtf8(output)},
      {"error", text::SanitizeUtf8(error)},
      {"exit_code", exit_code},
  };
  // Named only when there is a reason: a result that ran, or was refused by its command,
  // carries no refusal field and reads as it always did.
  if (refusal == Refusal::kNobodyToAsk) j.as_object()["refusal"] = "nobody_to_ask";
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
      const std::string refusal = json::ValueOrDefault<std::string>(j, "refusal", "");
      r.refusal = refusal == "nobody_to_ask" ? Refusal::kNobodyToAsk
                  : r.blocked                  ? Refusal::kPolicy
                                               : Refusal::kNone;
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
