// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>

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

}  // namespace pu::tools
