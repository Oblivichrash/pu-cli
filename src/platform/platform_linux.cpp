// SPDX-License-Identifier: GPL-3.0-only
#include "platform.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace pu::platform {

namespace {
std::atomic<bool> interrupted{false};

void SignalHandler(int) { interrupted = true; }
}  // namespace

void SetupSignalHandler() {
  struct sigaction sa{};
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
}

bool IsInterrupted() { return interrupted; }
void ClearInterruptFlag() { interrupted = false; }

int ExecuteCommand(const std::string& command, std::string& output) {
  std::string full_cmd = command + " 2>&1";
  FILE* pipe = popen(full_cmd.c_str(), "r");
  if (!pipe) { output = "popen failed"; return -1; }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
    output += buffer.data();

  int status = pclose(pipe);
  if (status == -1) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace pu::platform
