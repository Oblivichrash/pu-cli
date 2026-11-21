// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "pu/core/base.hpp"

namespace pu::platform {

// A child on a console follows the console output code page (cmd.exe).
std::string FromConsoleOutput(std::string_view text);

// A child on a pipe follows the ANSI code page, the locale encoding on Windows.
std::string FromPipedOutput(std::string_view text);

void SetupSignalHandler();

// True once the process has been asked to exit, by a signal or by the UI. It is
// never cleared: a process that is shutting down does not start another turn.
bool ShutdownRequested();

// The same request as a token, for the call sites that take a CancelToken.
CancelToken& ShutdownToken();

// This process's identity, and whether a process with that identity is still running.
int CurrentProcessId();
bool ProcessRunning(int pid);

// Whether standard input is a terminal a reader can answer on, as opposed to a pipe, a file
// or /dev/null. A prompt is only worth printing where the answer can arrive: asked of a pipe
// that has none, it waits for a keyboard that is not there.
bool StdinIsTerminal();

// One line from a reader, asked at `prompt` (printed to stderr) with `initial` shown beside it
// in brackets. A blank line takes the default and any other line is the answer, which is the
// shape every shell prompt has: the reader types the whole value or none of it, and nothing
// here edits the line on their behalf.
//
// The terminal is left as it was found -- no mode is set, so the reader's own line editing,
// keymap and history all work as they do everywhere else on the machine.
//
// Returns `initial` at end of input -- the answer a blank line gives -- and false when there
// was no input at all, which is the one case a caller cannot tell from an accepted default.
std::pair<bool, std::string> ReadLineWithDefault(const std::string& prompt,
                                                 const std::string& initial);

// A stamp that names this particular holder of `pid`, not merely the number. A pid is reused once
// its owner exits, so a record that stores one and asks later whether "the process" is alive gets
// a true answer about a different program. The stamp is the process's start time, and it is
// stable for the life of one process, empty when the platform cannot say.
std::string ProcessStartToken(int pid);

}  // namespace pu::platform
