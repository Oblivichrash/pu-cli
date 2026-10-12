// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace pu::platform {

// Runs a shell command with stderr merged via `2>&1`; -1 when the shell could not run.
int ExecuteCommand(const std::string& command, std::string& output,
                   const std::string& working_dir = {});

// A child on a console follows the console output code page (cmd.exe).
std::string FromConsoleOutput(std::string_view text);

// A child on a pipe follows the ANSI code page, the locale encoding on Windows.
std::string FromPipedOutput(std::string_view text);

void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

}  // namespace pu::platform
