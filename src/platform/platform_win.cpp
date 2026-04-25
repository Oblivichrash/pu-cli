// SPDX-License-Identifier: GPL-3.0-only

#include "platform.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace pu::platform {

namespace {

std::atomic<bool> interrupted{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT) {
    interrupted = true;
    return TRUE;
  }
  return FALSE;
}

const std::vector<std::string> dangerous_patterns = {
    R"(rm\s+-rf\s+/)",
    R"(sudo\b)",
    R"(mkfs)",
    R"(dd\s+if=.*of=/dev/sd)",
    R"(:\(\)\{ :\|:&\};:)",
};

}  // namespace

void SetupSignalHandler() {
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
}

bool IsInterrupted() { return interrupted; }
void ClearInterruptFlag() { interrupted = false; }

int ExecuteCommand(const std::string& command, std::string& output) {
  std::string full_cmd = command + " 2>&1";
  FILE* pipe = _popen(full_cmd.c_str(), "r");
  if (!pipe) {
    output = "popen failed";
    return -1;
  }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

  int status = _pclose(pipe);
  return (status == -1) ? -1 : status;
}

bool IsDangerous(const std::string& command, std::string* reason) {
  for (const auto& pattern : dangerous_patterns) {
    try {
      std::regex re(pattern, std::regex::icase);
      if (std::regex_search(command, re)) {
        if (reason) {
          *reason = "Matches dangerous pattern: " + pattern;
        }
        return true;
      }
    } catch (const std::regex_error&) {
      // ignore malformed patterns
    }
  }
  return false;
}

}  // namespace pu::platform
