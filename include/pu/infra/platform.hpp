// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

namespace pu::platform {

int ExecuteCommand(const std::string& command, std::string& output);

/**
 * Execute a command safely without invoking a shell.
 * @param command The command string (will be parsed into argv array)
 * @param output  The stdout/stderr output of the command
 * @return Exit code of the child process, or -1 on error
 */
int ExecuteCommandSafe(const std::string& command, std::string& output);

/**
 * Execute a command with explicit arguments, bypassing shell entirely.
 * @param argv  Null-terminated array of arguments (argv[0] is the program)
 * @param output  The stdout/stderr output of the command
 * @return Exit code of the child process, or -1 on error
 */
int ExecuteCommandArgv(const std::vector<std::string>& argv, std::string& output);

void SetupSignalHandler();
bool IsInterrupted();
void ClearInterruptFlag();

}  // namespace pu::platform