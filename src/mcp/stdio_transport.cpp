// SPDX-License-Identifier: GPL-3.0-only
#include "pu/mcp/stdio_transport.hpp"

#include <spdlog/spdlog.h>
#include <cstring>
#include <cerrno>

#ifdef _WIN32
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

StdioTransport::StdioTransport(const std::string& command, const std::vector<std::string>& args)
    : command_(command), args_(args) {}

StdioTransport::~StdioTransport() { Stop(); }

bool StdioTransport::SpawnProcess() {
#ifdef _WIN32
  // Windows support is a stub; full implementation requires CreateProcess + pipes.
  // Log a warning and return failure so the caller can skip this server.
  spdlog::warn("MCP stdio transport not implemented on Windows");
  return false;
#else
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

  if (pid_ == 0) {  // child
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
  } else {  // parent
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];
    return true;
  }
#endif
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

#ifdef _WIN32
  if (stdin_write_ != INVALID_HANDLE_VALUE) {
    CloseHandle(stdin_write_);
    stdin_write_ = INVALID_HANDLE_VALUE;
  }
  if (stdout_read_ != INVALID_HANDLE_VALUE) {
    CloseHandle(stdout_read_);
    stdout_read_ = INVALID_HANDLE_VALUE;
  }
  if (process_handle_ != INVALID_HANDLE_VALUE) {
    TerminateProcess(process_handle_, 1);
    CloseHandle(process_handle_);
    process_handle_ = INVALID_HANDLE_VALUE;
  }
#else
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
#endif
}

void StdioTransport::ReaderLoop() {
#ifdef _WIN32
  spdlog::warn("MCP reader loop not implemented on Windows");
  running_ = false;
  return;
#else
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
      std::string line = chunk.substr(0, pos);
      chunk.erase(0, pos + 1);
      if (!line.empty() && on_message_) {
        on_message_(line);
      }
    }
    leftover += chunk;
  }
  running_ = false;
#endif
}

bool StdioTransport::WriteLine(const std::string& line) {
#ifdef _WIN32
  spdlog::warn("MCP WriteLine not implemented on Windows");
  return false;
#else
  if (stdin_fd_ == -1) return false;
  std::string out = line + "\n";
  ssize_t written = write(stdin_fd_, out.c_str(), out.size());
  return (written == static_cast<ssize_t>(out.size()));
#endif
}

}  // namespace pu::mcp
