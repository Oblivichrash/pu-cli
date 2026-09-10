// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

namespace pu::platform {

// Runs a full shell command (popen/_popen) with stderr merged via `2>&1`.
// Returns the shell exit code, or -1 on error.
int ExecuteCommand(const std::string& command, std::string& output);

void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

}  // namespace pu::platform
