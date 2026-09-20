// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/platform.hpp"

#include "pu/core/text.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
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

#ifdef _WIN32
namespace {

std::wstring ToWide(const std::string& text, UINT code_page) {
  const int length = MultiByteToWideChar(code_page, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(code_page, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), length);
  return wide;
}

std::string ToUtf8(const std::wstring& wide) {
  const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
  if (length <= 0) return {};
  std::string utf8(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      utf8.data(), length, nullptr, nullptr);
  return utf8;
}

// cmd.exe encodes redirected output with the console's output code page, while
// tools such as git and python emit UTF-8 whatever the console setting is. Only
// bytes that are not already valid UTF-8 are decoded.
std::string ConsoleOutputToUtf8(const std::string& raw) {
  if (pu::text::IsValidUtf8(raw)) return raw;

  const UINT console_code_page = GetConsoleOutputCP();
  const UINT code_page = console_code_page != 0 ? console_code_page : GetOEMCP();

  const std::string utf8 = ToUtf8(ToWide(raw, code_page));
  return utf8.empty() ? pu::text::SanitizeUtf8(raw) : utf8;
}

}  // namespace
#endif

int ExecuteCommand(const std::string& command, std::string& output,
                   const std::string& working_dir) {
  std::string full_cmd;
  if (working_dir.empty()) {
    full_cmd = command;
  } else {
    const auto quoted_dir = std::filesystem::path(working_dir).string();
#ifdef _WIN32
    full_cmd = "cd /d \"" + quoted_dir + "\" && (" + command + ")";
#else
    full_cmd = "cd \"" + quoted_dir + "\" && (" + command + ")";
#endif
  }
  full_cmd += " 2>&1";

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
  output = ConsoleOutputToUtf8(output);
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
