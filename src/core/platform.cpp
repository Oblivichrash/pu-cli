// SPDX-License-Identifier: GPL-3.0-only

#include "pu/core/platform.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "pu/core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pu::platform {

std::atomic<bool> interrupted{false};  // extern-declared in tests/mocks/serve_harness.hpp

void SetupSignalHandler() {
#ifdef _WIN32
  if (!SetConsoleCtrlHandler(
          [](unsigned long type) {
            if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
              interrupted = true;
              return TRUE;
            }
            return FALSE;
          },
          TRUE)) {
    spdlog::warn("SetConsoleCtrlHandler failed");
  }
#else
  struct sigaction sa {};
  sa.sa_handler = [](int) { interrupted = true; };
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
#endif
}

bool IsInterrupted() { return interrupted; }
void ClearInterruptFlag() { interrupted = false; }

std::string FromConsoleOutput(std::string_view text) {
#ifdef _WIN32
  UINT code_page = GetConsoleOutputCP();

  int wide_len = MultiByteToWideChar(code_page, 0, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(wide_len), L'\0');
  MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                      wide_len);

  int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, nullptr, 0, nullptr,
                                     nullptr);
  std::string utf8(static_cast<size_t>(utf8_len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, utf8.data(), utf8_len, nullptr,
                      nullptr);
  return utf8;
#else
  return text::SanitizeUtf8(text);
#endif
}

std::string FromPipedOutput(std::string_view text) {
#ifdef _WIN32
  UINT code_page = GetACP();

  int wide_len = MultiByteToWideChar(code_page, 0, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(wide_len), L'\0');
  MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                      wide_len);

  int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, nullptr, 0, nullptr,
                                     nullptr);
  std::string utf8(static_cast<size_t>(utf8_len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, utf8.data(), utf8_len, nullptr,
                      nullptr);
  return utf8;
#else
  return text::SanitizeUtf8(text);
#endif
}

#ifndef _WIN32

namespace {

}  // namespace

int ExecuteCommand(const std::string& command, std::string& output,
                   const std::string& working_dir) {
  CancelToken no_stop;
  return ExecuteCommand(command, output, no_stop, working_dir);
}

int ExecuteCommand(const std::string& command, std::string& output,
                   const CancelToken& stop_token, const std::string& working_dir) {
  std::string full_cmd;
  if (!working_dir.empty()) {
    full_cmd = "cd \"" + std::filesystem::path(working_dir).string() + "\" && (" + command + ")";
  } else {
    full_cmd = command;
  }
  // The merged stream is the tool's whole output; the caller cannot tell stderr
  // from stdout afterward, which is why the tool result carries one field for it.
  full_cmd += " 2>&1";

  auto start = std::chrono::steady_clock::now();

  const bool stops = static_cast<bool>(stop_token);

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    output = "pipe failed";
    spdlog::debug("[ExecuteCommand] pipe failed for command: {}", command);
    return -1;
  }

  // setpgid in the child detaches it from this process's group, so a kill of the
  // group takes the shell and any grandchildren with it.
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    output = "fork failed";
    spdlog::debug("[ExecuteCommand] fork failed for command: {}", command);
    return -1;
  }
  if (pid == 0) {
    setpgid(0, 0);
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[1]);
    execl("/bin/sh", "sh", "-c", full_cmd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  close(pipe_fds[1]);
  setpgid(pid, pid);  // raced with the child's own call; idempotent either way

  // The stream is fdopen'd, not popen'd, so pclose is not an option: it answers for
  // popen's pid table only, and on this stream returns 0 regardless of the child.
  // fclose closes the descriptor and waitpid reaps the child explicitly.
  FILE* pipe = fdopen(pipe_fds[0], "r");
  if (pipe == nullptr) {
    close(pipe_fds[0]);
    output = "fdopen failed";
    return -1;
  }

  // fgets parks on the pipe until the child writes, so a quiet command would keep
  // the token unchecked until it finished. The read runs on a poll tick instead:
  // output when there is any, the token when there is not.
  bool stopped = false;
  for (;;) {
    struct pollfd pfd = {pipe_fds[0], POLLIN, 0};
    const int ready = poll(&pfd, 1, stops ? 50 : -1);
    if (ready > 0) {
      char buffer[256];
      const ssize_t n = read(pipe_fds[0], buffer, sizeof buffer - 1);
      if (n <= 0) break;
      output.append(buffer, static_cast<size_t>(n));
      if (stops && stop_token->load(std::memory_order_acquire)) break;
    } else if (stops && stop_token->load(std::memory_order_acquire)) {
      break;
    }
  }

  if (stops && stop_token->load(std::memory_order_acquire)) {
    // The group kill takes the shell and any grandchildren with it.
    kill(-pid, SIGKILL);
    stopped = true;
  }

  fclose(pipe);
  int status = 0;
  waitpid(pid, &status, 0);

  int exit_code = -1;
  if (!stopped && status != -1 && WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  }

  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  // The command and its byte count are the whole story at debug level; the output
  // itself reaches the transcript, and a full copy here grows the file sink alone.
  spdlog::debug(
      "[ExecuteCommand] command='{}' exit_code={} elapsed_ms={} output_bytes={} stopped={}",
      command, exit_code, elapsed_ms, output.size(), stopped);
  return exit_code;
}

#else  // _WIN32

int ExecuteCommand(const std::string& command, std::string& output,
                   const CancelToken& stop_token, const std::string& working_dir) {
  // Windows keeps the popen path; a stop token is honored only between reads of the
  // merged stream, which is no worse than what the stream reader does with cancel.
  std::string full_cmd;
  if (!working_dir.empty()) {
    full_cmd = "cd /d \"" + std::filesystem::path(working_dir).string() + "\" && (" + command + ")";
  } else {
    full_cmd = command;
  }
  full_cmd += " 2>&1";

  auto start = std::chrono::steady_clock::now();

  FILE* pipe = _popen(full_cmd.c_str(), "r");
  if (!pipe) {
    output = "popen failed";
    spdlog::debug("[ExecuteCommand] popen failed for command: {}", command);
    return -1;
  }

  std::array<char, 256> buffer{};
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
    if (stop_token && stop_token->load(std::memory_order_acquire)) break;
  }

  output = FromConsoleOutput(output);
  int status = _pclose(pipe);
  int exit_code = (status == -1) ? -1 : status;

  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  spdlog::debug("[ExecuteCommand] command='{}' exit_code={} elapsed_ms={} output_bytes={}", command,
                exit_code, elapsed_ms, output.size());
  return exit_code;
}

#endif

}  // namespace pu::platform
