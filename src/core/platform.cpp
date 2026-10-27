// SPDX-License-Identifier: GPL-3.0-only

// Signals, the encoding a child's output arrives in, and what this end of the process looks
// like. Running a command lives in exec/run.cpp: nothing here touches a pipe.

#include "pu/core/platform.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>

#include "pu/core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace pu::platform {

namespace {

#ifndef _WIN32
// Linux would answer at /proc/self/stat, which /proc resolves to the caller, so one path serves
// both the process's own stamp and another process's.
std::string StartedAtTicks(const std::string& stat_line) {
  // The field before this one is the executable's name in parentheses, and a name may contain
  // spaces and both kinds of bracket, so the fields are counted from the last ')' rather than
  // from the start of the line.
  const std::size_t close = stat_line.rfind(')');
  if (close == std::string::npos) return {};
  std::istringstream rest(stat_line.substr(close + 1));
  // The next field is 3 (state), so `starttime`, which is 22, is the twentieth read from here:
  // boot-relative ticks, which is exactly the discriminator, since two processes that share a
  // pid number were started at different times.
  std::string field;
  for (int index = 3; index <= 22; ++index) {
    if (!(rest >> field)) return {};
  }
  return field;
}
#endif

}  // namespace

CancelToken& ShutdownToken() {
  static CancelToken token = std::make_shared<std::atomic<bool>>(false);
  return token;
}

void SetupSignalHandler() {
#ifdef _WIN32
  if (!SetConsoleCtrlHandler(
          [](unsigned long type) {
            if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
              ShutdownToken()->store(true, std::memory_order_release);
              return TRUE;
            }
            return FALSE;
          },
          TRUE)) {
    spdlog::warn("SetConsoleCtrlHandler failed");
  }
#else
  struct sigaction sa {};
  sa.sa_handler = [](int) {
    static std::atomic<bool>* raw = ShutdownToken().get();
    raw->store(true, std::memory_order_release);
  };
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
#endif
}

bool ShutdownRequested() { return ShutdownToken()->load(std::memory_order_acquire); }

int CurrentProcessId() {
#ifdef _WIN32
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(::getpid());
#endif
}

std::string ProcessStartToken(int pid) {
  if (pid <= 0) return {};
#if defined(_WIN32)
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               static_cast<DWORD>(pid));
  if (process == nullptr) return {};
  FILETIME created{}, exited{}, kernel{}, user{};
  std::string stamp;
  if (GetProcessTimes(process, &created, &exited, &kernel, &user) != 0) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = created.dwLowDateTime;
    ticks.HighPart = created.dwHighDateTime;
    stamp = std::to_string(ticks.QuadPart);
  }
  CloseHandle(process);
  return stamp;
#else
  // macOS keeps no /proc and answers with an empty stamp, which is the documented answer when the
  // platform cannot say: the reader then falls back to the pid, which is what it did before a
  // start time existed. Linux resolves /proc/self to the caller, so one path serves both this
  // process's stamp and another process's.
  std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
  if (!file.is_open()) return {};
  std::string line;
  std::getline(file, line);
  return StartedAtTicks(line);
#endif
}

bool ProcessRunning(int pid) {
  if (pid <= 0) return false;
#ifdef _WIN32
  // A pid stays reserved while any handle to the process is open, so the signal is the exit
  // code being readable rather than the handle having opened: a stopped server nobody joined
  // still has a handle, and reads as stopped the moment it exits.
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                               static_cast<DWORD>(pid));
  if (process == nullptr) return false;
  DWORD exit_code = 0;
  const bool alive = GetExitCodeProcess(process, &exit_code) != 0 && exit_code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
#else
  // A child that has exited but not been reaped is this process's own to wait for, and only
  // its own: anyone else probing it gets ESRCH, which is the right answer for a record left
  // by a process that is gone.
  errno = 0;
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno == EPERM;
#endif
}

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

}  // namespace pu::platform
