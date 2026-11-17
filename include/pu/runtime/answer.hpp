// SPDX-License-Identifier: GPL-3.0-only
//
// What a finished turn says, and where it says it.
//
// The answer goes to stdout and everything that is not the answer goes to stderr: a notice
// about a retry, what the turn cost, why it failed. That split is the reason a caller can
// redirect the output of `pu ask` and get the answer alone, so it is a rule worth stating
// once rather than arranging by hand in every command that takes a turn.
//
// This lives beside `ExecutionResult` rather than in `src/app/` because more than one front
// end takes a turn — the terminal and the browser both do — and they should not disagree
// about which of a turn's outcomes counts as the answer.

#pragma once

#include <iosfwd>

#include "pu/runtime/executor.hpp"

namespace pu::runtime {

// The turn's answer, on `out`. A streamed answer already reached the caller a token at a
// time and wants only its closing newline; a command's own output is the answer and wants
// no second one; a turn that said nothing still ends the line, so a prompt and a reply
// cannot run together.
void PrintAnswer(const ExecutionResult& result, std::ostream& out);

// Why the turn failed, on `err`. Printed once: a failure already reported by the executor
// as a notice is not repeated here.
void PrintFailure(const ExecutionResult& result, std::ostream& err);

}  // namespace pu::runtime
