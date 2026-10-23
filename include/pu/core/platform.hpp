// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "pu/core/base.hpp"

namespace pu::platform {

// Runs a shell command with stderr merged via `2>&1`; -1 when the shell could not run.
int ExecuteCommand(const std::string& command, std::string& output,
                   const std::string& working_dir = {});

// Same, with a stop token: when it becomes true the command's process group is
// killed and the call returns with what the command had printed so far. The caller
// tells a kill from an ordinary exit by the token, not by the exit code.
int ExecuteCommand(const std::string& command, std::string& output,
                   const CancelToken& stop_token, const std::string& working_dir = {});

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

// A stamp that names this particular holder of `pid`, not merely the number. A pid is reused once
// its owner exits, so a record that stores one and asks later whether "the process" is alive gets
// a true answer about a different program. The stamp is the process's start time, and it is
// stable for the life of one process, empty when the platform cannot say.
std::string ProcessStartToken(int pid);

// The C library each platform reads the environment through keeps its own copy: glibc reads what
// it captured at startup until the first setenv(), and POSIX says in turn that writing environ
// directly leaves getenv() behind. A process that calls both -- a test that scopes a variable for
// one case, a launcher that sets one before a read -- therefore has to say which copy everyone
// shares. Constructing this moves Linux onto the library's copy; on Windows there is one copy, so
// it does nothing. Nothing needs to inspect it, so it is only there for its lifetime.
class LiveEnvironment {
 public:
  LiveEnvironment();
  LiveEnvironment(const LiveEnvironment&) = delete;
  LiveEnvironment& operator=(const LiveEnvironment&) = delete;
};

}  // namespace pu::platform
