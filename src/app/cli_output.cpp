// SPDX-License-Identifier: GPL-3.0-only
#include "app/cli_output.hpp"

#include <iostream>
#include <sstream>
#include <string>

#include "pu/core/platform.hpp"

namespace pu::cli::detail {

// Notices are diagnostics, so they go to stderr: stdout carries what was asked for and
// nothing else, which is what keeps `pu ask "..." | ...` usable.
void PrintNotice(const ExecutionResult& result) {
  if (result.notice.empty()) return;
  if (result.was_streamed) std::cerr << "\n";
  std::cerr << result.notice << "\n";
}

// What the turn cost, for a caller that asked. Written on stderr with the other diagnostics:
// stdout is the answer, and a number beside it would make the answer the second field of a
// record nobody agreed to.
void PrintUsage(const ExecutionResult& result) {
  if (!result.usage) return;
  const TokenUsage& usage = *result.usage;

  std::ostringstream line;
  line << "usage: " << usage.prompt_tokens << " prompt, " << usage.completion_tokens
       << " completion";
  if (usage.cached_tokens > 0) line << ", " << usage.cached_tokens << " cached";
  if (usage.total_tokens > 0) line << ", " << usage.total_tokens << " total";
  // A price only where the product reported one, and named in that product's unit: two
  // products' units are not comparable, so the unit is printed rather than assumed.
  if (usage.cost) {
    line << ", " << usage.cost->amount;
    if (!usage.cost->unit.empty()) line << " " << usage.cost->unit;
  }
  if (result.rounds > 1) line << " over " << result.rounds << " provider calls";
  std::cerr << line.str() << "\n";
}

// The failed turn's message, printed once. The executor records it rather than logging it,
// so the front end decides whether it is a line here or a field somewhere else.
void PrintFailure(const ExecutionResult& result) {
  if (!result.has_error) return;
  std::cerr << (result.error_message.empty() ? "Request failed" : result.error_message) << "\n";
}

// The exit status a finished turn answers with. A command the router answered is not a
// provider failure: `pu ask "/help"` did what it was asked.
ExitCode ExitCodeForAsk(const ExecutionResult& result) {
  // A person stopped the process, which is neither a failure to retry nor one to fix. The
  // token is global, so it is asked rather than threaded through every result.
  if (platform::ShutdownRequested()) return ExitCode::kInterrupted;
  // A command is answered without asking a provider, so its failure is not a provider
  // failure either -- it is the line itself, which the caller typed. `kUsage` is the status
  // that names a mistake in the input; `kOk` is the one that says the router answered.
  if (result.was_command) {
    return result.has_error ? ExitCode::kUsage : ExitCode::kOk;
  }
  // The sandbox refused everything the turn tried. Retrying cannot help, but what has to
  // change differs: a command or a policy for one, a front end that cannot ask for the
  // other.
  if (result.only_unaskable_calls) return ExitCode::kUsage;
  if (result.only_blocked_calls) return ExitCode::kSandbox;
  if (!result.has_error) return ExitCode::kOk;
  return ExitCode::kProvider;
}

}  // namespace pu::cli::detail
