// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/policy.hpp"

#include <regex>
#include <vector>

namespace pu::tools {

namespace {

bool MatchesAny(const std::string& command, const std::vector<std::string>& patterns,
                std::string* matched) {
  for (const auto& pattern : patterns) {
    try {
      if (std::regex_search(command, std::regex(pattern, std::regex::icase))) {
        if (matched != nullptr) *matched = pattern;
        return true;
      }
    } catch (const std::regex_error&) {
    }
  }
  return false;
}

// Ought to be refused whoever asks, and therefore needs no policy to say so.
const std::vector<std::string> kDangerous = {
    R"(rm\s+-rf\s+/)", R"(sudo\b)", R"(mkfs)", R"(dd\s+if=.*of=/dev/sd)", R"(:\(\)\{ :\|:&\};:)"};

// Read-only tools, which need no approval. A pattern rather than a word so `grep` and
// `grep -r` land on the same side.
const std::vector<std::string> kSafe = {
    "ls",   "pwd",    "cat",      "head",   "tail",  "less", "more",   "echo",
    "date", "whoami", "hostname", "uptime", "which", "type", "wc",     "sort",
    "uniq", "cut",    "tr",       "find .", "grep",  "awk",  "sed",    "diff",
    "file", "stat",   "du",       "df",     "free",  "ps",   "top -n", "pgrep"};

}  // namespace

Assessment Assess(std::string_view command, const config::SecurityPolicy* policy) {
  const std::string text(command);

  std::string pattern;
  if (MatchesAny(text, kDangerous, &pattern)) {
    return {Risk::kDangerous, "matches dangerous pattern: " + pattern};
  }
  if (policy != nullptr && !policy->approval_patterns.empty() &&
      MatchesAny(text, policy->approval_patterns, &pattern)) {
    return {Risk::kApproval, {}};
  }
  return {MatchesAny(text, kSafe, nullptr) ? Risk::kSafe : Risk::kNeutral, {}};
}

}  // namespace pu::tools
