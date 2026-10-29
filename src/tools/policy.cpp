// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/policy.hpp"

#include <regex>
#include <vector>

namespace pu::tools {

namespace {

// A pattern and the expression the compiler made of it. Compiled once, because a regex is
// not a string: `std::regex_search` on a fresh `std::regex` rebuilds the machine on every
// call, and this runs on every command.
struct Rule {
  std::string pattern;
  std::regex expression;
};

std::vector<Rule> Compile(const std::vector<std::string>& patterns) {
  std::vector<Rule> rules;
  rules.reserve(patterns.size());
  for (const auto& pattern : patterns) {
    // A pattern the engine refuses is dropped rather than thrown: it arrives from a file, and
    // one malformed line must not turn every command into a crash.
    try {
      rules.push_back({pattern, std::regex(pattern, std::regex::icase)});
    } catch (const std::regex_error&) {
    }
  }
  return rules;
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

const std::vector<Rule>& DangerousRules() {
  static const std::vector<Rule> rules = Compile(kDangerous);
  return rules;
}

const std::vector<Rule>& SafeRules() {
  static const std::vector<Rule> rules = Compile(kSafe);
  return rules;
}

bool MatchesAny(const std::string& command, const std::vector<Rule>& rules, std::string* matched) {
  for (const auto& rule : rules) {
    if (std::regex_search(command, rule.expression)) {
      if (matched != nullptr) *matched = rule.pattern;
      return true;
    }
  }
  return false;
}

}  // namespace

bool MatchesAnyPattern(std::string_view command, const std::vector<std::string>& patterns) {
  const std::string text(command);
  for (const auto& pattern : patterns) {
    try {
      if (std::regex_search(text, std::regex(pattern, std::regex::icase))) return true;
    } catch (const std::regex_error&) {
    }
  }
  return false;
}

Assessment Assess(std::string_view command, const config::SecurityPolicy* policy) {
  const std::string text(command);

  std::string pattern;
  if (MatchesAny(text, DangerousRules(), &pattern)) {
    return {Risk::kDangerous, "matches dangerous pattern: " + pattern};
  }
  if (policy != nullptr && !policy->approval_patterns.empty() &&
      MatchesAnyPattern(text, policy->approval_patterns)) {
    return {Risk::kApproval, {}};
  }
  return {MatchesAny(text, SafeRules(), nullptr) ? Risk::kSafe : Risk::kNeutral, {}};
}

}  // namespace pu::tools
