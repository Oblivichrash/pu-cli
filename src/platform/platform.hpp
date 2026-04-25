// SPDX-License-Identifier: GPL-3.0-only
//
// Platform abstraction for command execution and signal handling.

#pragma once

#include <string>

namespace pu::platform {

// Execute a shell command and capture its combined stdout/stderr output.
// Returns the exit code and fills `output` with the captured text.
int ExecuteCommand(const std::string& command, std::string& output);

// Check if a command matches dangerous patterns.
bool IsDangerous(const std::string& command, std::string* reason = nullptr);

// Signal handling for asynchronous interruption.
// Must be called once at program startup.
void SetupSignalHandler();

// Returns true if an interrupt signal has been received.
bool IsInterrupted();

// Reset the interrupt flag (e.g., at the start of a new request).
void ClearInterruptFlag();

}  // namespace pu::platform
