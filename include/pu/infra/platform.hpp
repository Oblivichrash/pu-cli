// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace pu::platform {

/**
 * Execute a full shell command (unified command execution entry point).
 *
 * Internally uses popen (POSIX) / _popen (Windows) with `2>&1` appended so that
 * stdout and stderr are merged into a single stream. All shell features are
 * supported: pipes, redirections, logical operators, and command sequences.
 *
 * @param command The complete shell command string
 * @param output  Merged stdout+stderr output of the command
 * @return Exit code of the shell (0 on success, non-zero on failure, -1 on error)
 */
int ExecuteCommand(const std::string& command, std::string& output);

void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

}  // namespace pu::platform
