// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace pu::mcp {

using MessageCallback = std::function<void(const std::string&)>;

class StdioTransport {
 public:
  StdioTransport(const std::string& command, const std::vector<std::string>& args);
  ~StdioTransport();

  StdioTransport(const StdioTransport&) = delete;
  StdioTransport& operator=(const StdioTransport&) = delete;

  bool Start(MessageCallback on_message);
  void Stop();
  bool WriteLine(const std::string& line);

  bool IsRunning() const { return running_; }

 private:
  bool SpawnProcess();
  void ReaderLoop();

  std::string command_;
  std::vector<std::string> args_;

#ifdef _WIN32
  HANDLE stdin_write_ = INVALID_HANDLE_VALUE;
  HANDLE stdout_read_ = INVALID_HANDLE_VALUE;
  HANDLE process_handle_ = INVALID_HANDLE_VALUE;
#else
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  pid_t pid_ = -1;
#endif

  std::thread reader_thread_;
  std::atomic<bool> running_{false};
  MessageCallback on_message_;
};

}  // namespace pu::mcp
