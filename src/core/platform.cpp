// SPDX-License-Identifier: GPL-3.0-only

// Signals, the encoding a child's output arrives in, and what this end of the process looks
// like. Running a command lives in exec/run.cpp: nothing here touches a pipe.

#include "pu/core/platform.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "pu/core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <termios.h>
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

bool StdinIsTerminal() {
#ifdef _WIN32
  // A console is a character device this process has one of; a pipe or a redirected file is
  // a handle that reports a type rather than a terminal.
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  if (input == nullptr || input == INVALID_HANDLE_VALUE) return false;
  DWORD mode = 0;
  return GetConsoleMode(input, &mode) != 0;
#else
  // `isatty` is asked of the descriptor rather than the stream: a reader answering a prompt
  // has one on stdin, and a redirected one has a file.
  return ::isatty(STDIN_FILENO) != 0;
#endif
}

#ifndef _WIN32
namespace {

// What a terminal was set to, put back by the destructor however the read leaves: a prompt
// that throws, or a signal that unwinds, does not leave the console in raw mode where the
// reader's own shell inherits it.
struct RawMode {
  termios saved{};
  bool active = false;

  RawMode() {
    if (::tcgetattr(STDIN_FILENO, &saved) != 0) return;
    termios raw = saved;
    // Byte at a time, no echo, no line buffering: every key arrives here at once so this
    // code, and not the terminal, decides what a backspace does to the line. SIGINT is held
    // back too: the caller's handler only raises a flag, so a ^C left to it would leave this
    // read waiting for a keyboard that has stopped answering. It arrives as a byte instead.
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
    active = true;
  }

  ~RawMode() {
    if (active) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
  }

  RawMode(const RawMode&) = delete;
  RawMode& operator=(const RawMode&) = delete;
};

// One key, or -1 at end of input. A multi-byte key is one key: an arrow arrives as ESC
// followed by two more bytes, and reading them here keeps them out of the line as text.
int ReadKey() {
  unsigned char byte = 0;
  const ssize_t count = ::read(STDIN_FILENO, &byte, 1);
  if (count <= 0) return -1;
  if (byte != 0x1b) return byte;
  // A lone ESC is a key of its own; one followed within a tenth of a second by more bytes is
  // the start of a sequence, and the rest of it is consumed so it never lands in the line.
  termios saved{};
  if (::tcgetattr(STDIN_FILENO, &saved) != 0) return 0x1b;
  termios wait = saved;
  wait.c_cc[VMIN] = 0;
  wait.c_cc[VTIME] = 1;
  ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &wait);
  unsigned char next = 0;
  const ssize_t got = ::read(STDIN_FILENO, &next, 1);
  if (got > 0 && (next == '[' || next == 'O')) {
    unsigned char third = 0;
    const ssize_t tail = ::read(STDIN_FILENO, &third, 1);
    (void)tail;
  }
  ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
  if (got <= 0) return 0x1b;
  // An editing key this line does not act on, or the ESC of a sequence: the same answer,
  // since neither is a character of the answer.
  return 0x100;
}

}  // namespace
#endif

std::pair<bool, std::string> ReadLineWithDefault(const std::string& prompt,
                                                 const std::string& initial) {
  if (!StdinIsTerminal()) {
    // Nothing may be echoed or edited on a stream, so the default goes out as text after the
    // prompt and a whole line is read: a script's answer arrives as it always did, and a blank
    // line is the default because that is what the bracketed text says. A question with no
    // default shows no brackets: there is nothing there to take.
    std::cerr << prompt;
    if (!initial.empty()) std::cerr << " [" << initial << ']';
    std::cerr << ": ";
    std::cerr.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return {false, initial};
    return {true, line.empty() ? initial : line};
  }
  std::cerr << prompt << ": " << initial;
  std::cerr.flush();

#ifdef _WIN32
  // The console has its own mode flags; kept to the same behaviour, the default is printed
  // and a whole line is read, so an answer typed on Windows is never edited either.
  std::string line;
  if (!std::getline(std::cin, line)) return {false, initial};
  return {true, line.empty() ? initial : line};
#else
  RawMode raw;
  if (!raw.active) {
    std::string line;
    if (!std::getline(std::cin, line)) return {false, initial};
    return {true, line.empty() ? initial : line};
  }

  std::string line = initial;
  for (;;) {
    const int key = ReadKey();
    if (key < 0) {  // end of input: the answer a blank line gives
      std::cerr << "\n";
      return {false, line};
    }
    if (key == '\r' || key == '\n') {
      std::cerr << "\n";
      return {true, line};
    }
    if (key == 0x03) {  // ^C is not an answer, and leaves as one who gave none
      std::cerr << "\n";
      return {false, initial};
    }
    if (key == 0x04) {  // ^D on a blank line is the end of input
      if (line.empty()) {
        std::cerr << "\n";
        return {false, line};
      }
      std::cerr << "\n";
      return {true, line};
    }
    if (key == 0x7f || key == 0x08) {  // backspace
      if (line.empty()) continue;
      // Erase the last character on the line as the terminal would: back, overwrite with
      // a space, back again. A multi-byte character is erased a byte at a time, which the
      // terminal shows as the whole glyph going.
      std::cerr << "\b \b";
      line.pop_back();
      continue;
    }
    if (key == 0x15) {  // ^U clears the line
      for (std::size_t i = 0; i < line.size(); ++i) std::cerr << "\b \b";
      line.clear();
      continue;
    }
    if (key > 0x1f && key < 0x7f) {
      line.push_back(static_cast<char>(key));
      std::cerr << static_cast<char>(key);
      continue;
    }
    // Anything else -- arrows, function keys, other control keys -- is a key this line does
    // not act on, and leaving it out keeps it out of the answer.
  }
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
