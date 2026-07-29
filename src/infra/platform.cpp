// SPDX-License-Identifier: GPL-3.0-only
#include "pu/infra/platform.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
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

// Parse a command string into an argv array, respecting single and double quotes.
static std::vector<std::string> SplitCommand(const std::string& cmd) {
  std::vector<std::string> args;
  std::string current;
  bool in_single_quotes = false;
  bool in_double_quotes = false;
  for (char c : cmd) {
    if (c == '\'' && !in_double_quotes) {
      in_single_quotes = !in_single_quotes;
    } else if (c == '"' && !in_single_quotes) {
      in_double_quotes = !in_double_quotes;
    } else if (c == ' ' && !in_single_quotes && !in_double_quotes) {
      if (!current.empty()) {
        args.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    args.push_back(current);
  }
  return args;
}

#ifdef _WIN32
// Windows stubs (to be fully implemented)
int ExecuteCommandSafe(const std::string& command, std::string& output) {
  (void)command;
  output = "ExecuteCommandSafe not yet implemented on Windows";
  return -1;
}

int ExecuteCommandArgv(const std::vector<std::string>& argv, std::string& output) {
  (void)argv;
  output = "ExecuteCommandArgv not yet implemented on Windows";
  return -1;
}
#else
// POSIX implementation using fork/execvp to avoid shell injection.

int ExecuteCommandArgv(const std::vector<std::string>& argv, std::string& output) {
  if (argv.empty()) {
    output = "Error: empty argv";
    return -1;
  }

  // execvp requires a null-terminated char* array.
  std::vector<const char*> exec_argv;
  exec_argv.reserve(argv.size() + 1);
  for (const auto& arg : argv) {
    exec_argv.push_back(arg.c_str());
  }
  exec_argv.push_back(nullptr);

  int pipe_fd[2];
  if (pipe(pipe_fd) == -1) {
    output = "pipe() failed: " + std::string(strerror(errno));
    return -1;
  }

  pid_t pid = fork();
  if (pid == -1) {
    output = "fork() failed: " + std::string(strerror(errno));
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    return -1;
  }

  if (pid == 0) {
    close(pipe_fd[0]);
    dup2(pipe_fd[1], STDOUT_FILENO);
    dup2(pipe_fd[1], STDERR_FILENO);
    close(pipe_fd[1]);

    execvp(exec_argv[0], const_cast<char* const*>(exec_argv.data()));
    // execvp only returns on error.
    _exit(127);
  } else {
    close(pipe_fd[1]);

    std::string result;
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(pipe_fd[0], buffer, sizeof(buffer) - 1)) > 0) {
      buffer[bytes_read] = '\0';
      result += buffer;
    }
    close(pipe_fd[0]);

    int status;
    waitpid(pid, &status, 0);

    output = result;
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    return -1;
  }
}

int ExecuteCommandSafe(const std::string& command, std::string& output) {
  auto args = SplitCommand(command);
  if (args.empty()) {
    output = "Error: empty command";
    return -1;
  }
  return ExecuteCommandArgv(args, output);
}

#endif

}  // namespace pu::platform