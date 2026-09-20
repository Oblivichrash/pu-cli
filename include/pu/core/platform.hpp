// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace pu::platform {

// Runs a full shell command (popen/_popen) with stderr merged via `2>&1`.
// Returns the shell exit code, or -1 on error.
int ExecuteCommand(const std::string& command, std::string& output,
				   const std::string& working_dir = {});

// Text captured from a child process is in whatever encoding that process chose,
// so these normalise it to UTF-8. Text that is already valid UTF-8 is returned
// unchanged, which covers runtimes that always emit UTF-8 (Node.js).

// A child attached to a console follows the console output code page (cmd.exe).
std::string FromConsoleOutput(std::string_view text);

// A child attached to a pipe follows the ANSI code page (Python uses the locale
// encoding whenever stdout is not a terminal).
std::string FromPipedOutput(std::string_view text);

void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

}  // namespace pu::platform
