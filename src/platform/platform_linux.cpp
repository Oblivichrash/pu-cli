// SPDX-License-Identifier: GPL-3.0-only

#include "platform.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace pu::platform {

namespace {

std::atomic<bool> interrupted{false};

void SignalHandler(int /*signum*/) {
  interrupted = true;
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
  if (!pipe) {
    output = "popen failed";
    return -1;
  }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

  int status = pclose(pipe);
  if (status == -1) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
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
