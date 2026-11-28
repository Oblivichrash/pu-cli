// SPDX-License-Identifier: GPL-3.0-only
//
// What a finished turn says, and where it says it.
//
// The answer goes to stdout and everything else to stderr, so a caller can redirect the output
// of `pu ask` and get the answer alone. This lives beside `ExecutionResult` rather than in
// `src/app/` so the terminal and the browser cannot disagree about which outcome is the answer.

#pragma once

#include <iosfwd>

#include "pu/runtime/executor.hpp"

namespace pu::runtime {

// The turn's answer, on `out`. A streamed answer already reached the caller a token at a time
// and wants only its closing newline; a command's own output wants no second one; a turn that
// said nothing still ends the line, so a prompt and a reply cannot run together.
void PrintAnswer(const ExecutionResult& result, std::ostream& out);

// Why the turn failed, on `err`. Printed once: a failure already reported by the executor as a
// notice is not repeated here.
void PrintFailure(const ExecutionResult& result, std::ostream& err);

}  // namespace pu::runtime
