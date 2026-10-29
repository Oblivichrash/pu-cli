// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "pu/config/security_policy.hpp"

namespace pu::tools {

// How much a command has to be checked before it may run. Named for the answer rather
// than for the command, because the same command is a different answer under a
// different policy.
enum class Risk { kSafe, kNeutral, kApproval, kDangerous };

struct Assessment {
  Risk risk = Risk::kSafe;
  std::string reason;  // why, when the reason is what the model needs to hear
};

// The one place a command is judged. `policy` may be null, which leaves only the
// built-in dangerous patterns: a command that deletes a filesystem is refused whoever
// asks.
Assessment Assess(std::string_view command, const config::SecurityPolicy* policy);

// Whether `command` matches any of `patterns`, case-insensitively. A pattern the engine
// refuses is skipped rather than thrown: they arrive from a file, and one malformed line
// must not decide that every command afterwards is an error. Also compiles them, so it
// belongs next to `Assess` rather than beside every caller that has its own list.
bool MatchesAnyPattern(std::string_view command, const std::vector<std::string>& patterns);

}  // namespace pu::tools
