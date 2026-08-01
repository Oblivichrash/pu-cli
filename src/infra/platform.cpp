// SPDX-License-Identifier: GPL-3.0-only
#include "pu/infra/platform.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
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

  auto start = std::chrono::steady_clock::now();

#ifdef _WIN32
  FILE* pipe = _popen(full_cmd.c_str(), "r");
#else
  FILE* pipe = popen(full_cmd.c_str(), "r");
#endif
  if (!pipe) {
    output = "popen failed";
    spdlog::debug("[ExecuteCommand] popen failed for command: {}", command);
    return -1;
  }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    output += buffer.data();

#ifdef _WIN32
  int status = _pclose(pipe);
  int exit_code = (status == -1) ? -1 : status;
#else
  int status = pclose(pipe);
  int exit_code;
  if (status == -1) {
    exit_code = -1;
  } else {
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
#endif

  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  spdlog::debug("[ExecuteCommand] command='{}' exit_code={} elapsed_ms={} output_bytes={}",
                command, exit_code, elapsed_ms, output.size());
  return exit_code;
}

}  // namespace pu::platform
