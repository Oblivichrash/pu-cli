// SPDX-License-Identifier: GPL-3.0-only
#include "infra/platform.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace pu::platform {

std::atomic<bool> interrupted{false};

#ifdef _WIN32
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT) { interrupted = true; return TRUE; }
  return FALSE;
}
void SetupSignalHandler() { SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE); }
#else
void SignalHandler(int) { interrupted = true; }
void SetupSignalHandler() {
  struct sigaction sa{};
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
}
#endif

bool IsInterrupted() { return interrupted; }
void ClearInterruptFlag() { interrupted = false; }

int ExecuteCommand(const std::string& command, std::string& output) {
  std::string full_cmd = command + " 2>&1";

#ifdef _WIN32
  FILE* pipe = _popen(full_cmd.c_str(), "r");
  if (!pipe) { output = "popen failed"; return -1; }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    output += buffer.data();

  int status = _pclose(pipe);
  return (status == -1) ? -1 : status;
#else
  FILE* pipe = popen(full_cmd.c_str(), "r");
  if (!pipe) { output = "popen failed"; return -1; }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    output += buffer.data();

  int status = pclose(pipe);
  if (status == -1) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace pu::platform
