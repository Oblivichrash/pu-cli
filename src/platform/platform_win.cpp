// SPDX-License-Identifier: GPL-3.0-only
#include "platform.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <regex>
#include <string>
#include <vector>
#include <windows.h>

namespace pu::platform {

namespace {
std::atomic<bool> interrupted{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT) { interrupted = true; return TRUE; }
  return FALSE;
}
}  // namespace

void SetupSignalHandler() { SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE); }
bool IsInterrupted() { return interrupted; }
void ClearInterruptFlag() { interrupted = false; }

int ExecuteCommand(const std::string& command, std::string& output) {
  std::string full_cmd = command + " 2>&1";
  FILE* pipe = _popen(full_cmd.c_str(), "r");
  if (!pipe) { output = "popen failed"; return -1; }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    output += buffer.data();

  int status = _pclose(pipe);
  return (status == -1) ? -1 : status;
}

}  // namespace pu::platform
