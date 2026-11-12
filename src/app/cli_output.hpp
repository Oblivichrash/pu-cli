// SPDX-License-Identifier: GPL-3.0-only
#ifndef PU_APP_CLI_OUTPUT_HPP
#define PU_APP_CLI_OUTPUT_HPP

// What a finished turn says and what it exits with, separated from the command line that
// produced it. These four functions read one `ExecutionResult` and nothing else -- no runtime,
// no options, no session -- so they live beside `cli.cpp` rather than inside it, where the
// argument parsing was making them hard to find.
//
// Internal to `src/app/`: not installed, and not part of the public header.

#include "pu/app/cli.hpp"
#include "pu/runtime/executor.hpp"

namespace pu::cli::detail {

// A diagnostic the turn produced (a retry, a notice), on stderr.
void PrintNotice(const ExecutionResult& result);

// What the turn cost, on stderr, when the caller asked for it.
void PrintUsage(const ExecutionResult& result);

// The failed turn's message, printed once, on stderr.
void PrintFailure(const ExecutionResult& result);

// The status a finished `ask` turn exits with.
ExitCode ExitCodeForAsk(const ExecutionResult& result);

}  // namespace pu::cli::detail

#endif  // PU_APP_CLI_OUTPUT_HPP
