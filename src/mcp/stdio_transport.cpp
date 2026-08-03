// SPDX-License-Identifier: GPL-3.0-only
#include "pu/mcp/stdio_transport.hpp"

#include <spdlog/spdlog.h>
#include <cstring>
#include <cerrno>
#include <algorithm>

#ifdef _WIN32
#  include <windows.h>
#  include <processthreadsapi.h>
#  include <fileapi.h>
#  include <handleapi.h>
#  include <synchapi.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#  include <fcntl.h>
#  include <csignal>
#endif

namespace pu::mcp {

#ifdef _WIN32

namespace {

struct ReaderContext {
  HANDLE stdout_read;
  std::atomic<bool>* running;
  MessageCallback on_message;
};

DWORD WINAPI ReaderThreadProc(LPVOID param) {
  auto* ctx = static_cast<ReaderContext*>(param);

  char buf[4096];
  std::string leftover;

  while (ctx->running->load(std::memory_order_acquire)) {
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(ctx->stdout_read, buf, sizeof(buf) - 1, &bytes_read, nullptr);
    if (!ok || bytes_read == 0) {
      DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA) break;
      if (err == ERROR_OPERATION_ABORTED || err == ERROR_INVALID_HANDLE) break;
      spdlog::error("MCP stdout ReadFile failed: {}", err);
      break;
    }

    buf[bytes_read] = '\0';
    std::string chunk(buf, bytes_read);

    size_t pos = 0;
    while ((pos = chunk.find('\n')) != std::string::npos) {
      std::string line = leftover + chunk.substr(0, pos);
      chunk.erase(0, pos + 1);
      leftover.clear();
      if (!line.empty() && ctx->on_message) ctx->on_message(line);
    }
    leftover += chunk;
  }

  if (!leftover.empty() && ctx->on_message) ctx->on_message(leftover);

  ctx->running->store(false, std::memory_order_release);
  return 0;
}

// CreateProcess takes a single command-line string; arguments with embedded
// spaces or quotes must be quoted and escaped.
std::string BuildCommandLine(const std::string& command,
                             const std::vector<std::string>& args) {
  auto quote = [](const std::string& s) -> std::string {
    if (s.find_first_of(" \"\t\n\v") == std::string::npos) return s;
    std::string escaped;
    escaped.reserve(s.size() + 4);
    escaped += '"';
    for (char c : s) {
      if (c == '"') escaped += "\\\"";
      else          escaped += c;
    }
    escaped += '"';
    return escaped;
  };

  std::string cmdline = quote(command);
  for (const auto& a : args) {
    cmdline += ' ';
    cmdline += quote(a);
  }
  return cmdline;
}

}  // namespace

StdioTransport::StdioTransport(const std::string& command,
                               const std::vector<std::string>& args)
    : command_(command), args_(args) {}

StdioTransport::~StdioTransport() { Stop(); }

bool StdioTransport::SpawnProcess() {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  HANDLE child_stdin_read = INVALID_HANDLE_VALUE;
  HANDLE child_stdin_write = INVALID_HANDLE_VALUE;
  HANDLE child_stdout_read = INVALID_HANDLE_VALUE;
  HANDLE child_stdout_write = INVALID_HANDLE_VALUE;
  HANDLE child_stderr_read = INVALID_HANDLE_VALUE;
  HANDLE child_stderr_write = INVALID_HANDLE_VALUE;

  auto close_handles = [&]() {
    for (HANDLE h : {child_stdin_read, child_stdin_write,
                     child_stdout_read, child_stdout_write,
                     child_stderr_read, child_stderr_write}) {
      if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
  };

  if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0)) {
    spdlog::error("CreatePipe (stdin) failed: {}", GetLastError());
    return false;
  }
  if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
    spdlog::error("SetHandleInformation (stdin write) failed: {}", GetLastError());
    close_handles();
    return false;
  }

  if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) {
    spdlog::error("CreatePipe (stdout) failed: {}", GetLastError());
    close_handles();
    return false;
  }
  if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
    spdlog::error("SetHandleInformation (stdout read) failed: {}", GetLastError());
    close_handles();
    return false;
  }

  if (!CreatePipe(&child_stderr_read, &child_stderr_write, &sa, 0)) {
    spdlog::error("CreatePipe (stderr) failed: {}", GetLastError());
    close_handles();
    return false;
  }
  if (!SetHandleInformation(child_stderr_read, HANDLE_FLAG_INHERIT, 0)) {
    spdlog::error("SetHandleInformation (stderr read) failed: {}", GetLastError());
    close_handles();
    return false;
  }

  PROCESS_INFORMATION pi{};
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.hStdInput = child_stdin_read;
  si.hStdOutput = child_stdout_write;
  si.hStdError = child_stderr_write;
  si.dwFlags |= STARTF_USESTDHANDLES;

  std::string cmdline = BuildCommandLine(command_, args_);
  std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
  cmdline_buf.push_back('\0');

  BOOL ok = CreateProcessA(
      nullptr, cmdline_buf.data(),
      nullptr, nullptr,
      TRUE, CREATE_NO_WINDOW,
      nullptr, nullptr,
      &si, &pi);

  if (!ok) {
    spdlog::error("CreateProcess failed for '{}': {}", command_, GetLastError());
    close_handles();
    return false;
  }

  CloseHandle(child_stdin_read);
  child_stdin_read = INVALID_HANDLE_VALUE;
  CloseHandle(child_stdout_write);
  child_stdout_write = INVALID_HANDLE_VALUE;
  CloseHandle(child_stderr_write);
  child_stderr_write = INVALID_HANDLE_VALUE;

  CloseHandle(pi.hThread);

  stdin_write_ = child_stdin_write;
  stdout_read_ = child_stdout_read;
  stderr_read_ = child_stderr_read;
  process_handle_ = pi.hProcess;
  return true;
}

bool StdioTransport::Start(MessageCallback on_message) {
  if (running_) return true;
  on_message_ = std::move(on_message);
  if (!SpawnProcess()) return false;
  running_ = true;

  auto* ctx = new ReaderContext{stdout_read_, &running_, on_message_};
  reader_thread_handle_ = CreateThread(
      nullptr, 0, ReaderThreadProc, ctx, 0, &reader_thread_id_);
  if (reader_thread_handle_ == INVALID_HANDLE_VALUE || !reader_thread_handle_) {
    spdlog::error("CreateThread for reader failed: {}", GetLastError());
    delete ctx;
    running_ = false;
    return false;
  }
  return true;
}

void StdioTransport::Stop() {
  running_ = false;

  if (stdin_write_ != INVALID_HANDLE_VALUE) {
    CloseHandle(stdin_write_);
    stdin_write_ = INVALID_HANDLE_VALUE;
  }

  if (reader_thread_handle_ != INVALID_HANDLE_VALUE) {
    DWORD wait_result = WaitForSingleObject(reader_thread_handle_, 2000);
    if (wait_result == WAIT_TIMEOUT) {
      spdlog::warn("MCP reader thread did not exit within 2s; terminating");
      TerminateThread(reader_thread_handle_, 1);
    }
    CloseHandle(reader_thread_handle_);
    reader_thread_handle_ = INVALID_HANDLE_VALUE;
  }

  if (stdout_read_ != INVALID_HANDLE_VALUE) {
    CloseHandle(stdout_read_);
    stdout_read_ = INVALID_HANDLE_VALUE;
  }
  if (stderr_read_ != INVALID_HANDLE_VALUE) {
    CloseHandle(stderr_read_);
    stderr_read_ = INVALID_HANDLE_VALUE;
  }

  if (process_handle_ != INVALID_HANDLE_VALUE) {
    DWORD wait_result = WaitForSingleObject(process_handle_, 3000);
    if (wait_result == WAIT_TIMEOUT) {
      spdlog::warn("MCP process did not exit within 3s; terminating");
      TerminateProcess(process_handle_, 1);
    }
    CloseHandle(process_handle_);
    process_handle_ = INVALID_HANDLE_VALUE;
  }
}

void StdioTransport::ReaderLoop() {}

bool StdioTransport::WriteLine(const std::string& line) {
  if (stdin_write_ == INVALID_HANDLE_VALUE) return false;
  std::string out = line + "\n";
  DWORD written = 0;
  BOOL ok = WriteFile(stdin_write_, out.data(),
                      static_cast<DWORD>(out.size()), &written, nullptr);
  if (!ok || written != static_cast<DWORD>(out.size())) {
    spdlog::error("MCP WriteFile failed: {}", GetLastError());
    return false;
  }
  return true;
}

#else  // _WIN32

StdioTransport::StdioTransport(const std::string& command,
                               const std::vector<std::string>& args)
    : command_(command), args_(args) {}

StdioTransport::~StdioTransport() { Stop(); }

bool StdioTransport::SpawnProcess() {
  int stdin_pipe[2], stdout_pipe[2];
  if (pipe(stdin_pipe) == -1 || pipe(stdout_pipe) == -1) {
    spdlog::error("pipe() failed: {}", strerror(errno));
    return false;
  }

  pid_ = fork();
  if (pid_ == -1) {
    spdlog::error("fork() failed: {}", strerror(errno));
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    return false;
  }

  if (pid_ == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    std::vector<const char*> exec_argv;
    exec_argv.push_back(command_.c_str());
    for (const auto& a : args_) exec_argv.push_back(a.c_str());
    exec_argv.push_back(nullptr);

    execvp(command_.c_str(), const_cast<char* const*>(exec_argv.data()));
    spdlog::error("execvp failed: {}", strerror(errno));
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  stdin_fd_ = stdin_pipe[1];
  stdout_fd_ = stdout_pipe[0];
  return true;
}

bool StdioTransport::Start(MessageCallback on_message) {
  if (running_) return true;
  on_message_ = std::move(on_message);
  if (!SpawnProcess()) return false;
  running_ = true;
  reader_thread_ = std::thread(&StdioTransport::ReaderLoop, this);
  return true;
}

void StdioTransport::Stop() {
  running_ = false;
  if (reader_thread_.joinable()) reader_thread_.join();

  if (stdin_fd_ != -1) {
    close(stdin_fd_);
    stdin_fd_ = -1;
  }
  if (stdout_fd_ != -1) {
    close(stdout_fd_);
    stdout_fd_ = -1;
  }
  if (pid_ > 0) {
    kill(pid_, SIGTERM);
    waitpid(pid_, nullptr, 0);
    pid_ = -1;
  }
}

void StdioTransport::ReaderLoop() {
  char buffer[4096];
  std::string leftover;
  while (running_) {
    ssize_t n = read(stdout_fd_, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
      if (n == -1 && errno == EINTR) continue;
      break;
    }
    buffer[n] = '\0';
    std::string chunk(buffer, n);
    size_t pos = 0;
    while ((pos = chunk.find('\n')) != std::string::npos) {
      std::string line = leftover + chunk.substr(0, pos);
      chunk.erase(0, pos + 1);
      leftover.clear();
      if (!line.empty() && on_message_) on_message_(line);
    }
    leftover += chunk;
  }
  running_ = false;
}

bool StdioTransport::WriteLine(const std::string& line) {
  if (stdin_fd_ == -1) return false;
  std::string out = line + "\n";
  ssize_t written = write(stdin_fd_, out.c_str(), out.size());
  return (written == static_cast<ssize_t>(out.size()));
}

#endif  // _WIN32

}  // namespace pu::mcp