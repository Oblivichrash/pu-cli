// SPDX-License-Identifier: GPL-3.0-only
#include "system.hpp"
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>
#include <vector>
#include <algorithm>

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

namespace pu::executor {

const std::vector<std::string> CommandExecutor::dangerous_patterns_ = {
    R"(rm\s+-rf\s+/)", R"(sudo\b)", R"(mkfs)", R"(dd\s+if=.*of=/dev/sd)", R"(:\(\)\{ :\|:&\};:)" };

const std::vector<std::string> CommandExecutor::safe_commands_ = {
    "ls", "pwd", "cat", "head", "tail", "less", "more",
    "echo", "date", "whoami", "hostname", "uptime",
    "which", "type", "wc", "sort", "uniq", "cut", "tr",
    "find .", "grep", "awk", "sed", "diff", "file",
    "stat", "du", "df", "free", "ps", "top -n", "pgrep" };

static bool MatchAnyPattern(const std::string& command, const std::vector<std::string>& patterns,
                            std::string* matched = nullptr) {
  for (const auto& pattern : patterns) {
    try {
      if (std::regex_search(command, std::regex(pattern, std::regex::icase))) {
        if (matched) *matched = pattern;
        return true;
      }
    } catch (const std::regex_error&) {}
  }
  return false;
}

CommandExecutor::CommandExecutor(std::string sandbox_path) : sandbox_path_(std::move(sandbox_path)) {}

RiskAssessment CommandExecutor::AssessRisk(const std::string& command) const {
  RiskAssessment result;
  std::string pattern;
  if (MatchAnyPattern(command, dangerous_patterns_, &pattern)) {
    result.level = RiskLevel::kDangerous;
    result.reason = "Matches dangerous pattern: " + pattern;
    return result;
  }
  result.level = MatchAnyPattern(command, safe_commands_) ? RiskLevel::kSafe : RiskLevel::kNeutral;
  return result;
}

ExecutionResult CommandExecutor::Execute(const std::string& command) {
  ExecutionResult result;
  auto risk = AssessRisk(command);
  if (risk.level == RiskLevel::kDangerous) {
    result.was_intercepted = true;
    result.intercept_reason = risk.reason;
    result.exit_code = -1;
    return result;
  }
  std::string full_command = sandbox_path_.empty() ? command : "cd " + sandbox_path_ + " && " + command;
  std::string output;
  int exit_code = pu::platform::ExecuteCommand(full_command, output);
  result.exit_code = exit_code;
  result.stdout_content = output;
  if (exit_code != 0) result.stderr_content = output;
  return result;
}

}  // namespace pu::executor
