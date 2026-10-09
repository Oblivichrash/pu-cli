// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#include "pu/mcp/transport.hpp"

namespace pu::mcp {

class StdioTransport : public Transport {
 public:
  StdioTransport(const std::string& command, const std::vector<std::string>& args);
  ~StdioTransport() override;

  StdioTransport(const StdioTransport&) = delete;
  StdioTransport& operator=(const StdioTransport&) = delete;

  bool Start(MessageCallback on_message) override;
  void Stop() override;
  bool WriteLine(const std::string& line) override;

 private:
  bool SpawnProcess();
  // The reader is a thread procedure on Windows, declared where it is defined, and a
  // member on POSIX, where it is joined.
#ifndef _WIN32
  void ReaderLoop();
#endif

  std::string command_;
  std::vector<std::string> args_;

#ifdef _WIN32
  HANDLE stdin_write_ = INVALID_HANDLE_VALUE;
  HANDLE stdout_read_ = INVALID_HANDLE_VALUE;
  HANDLE stderr_read_ = INVALID_HANDLE_VALUE;
  HANDLE process_handle_ = INVALID_HANDLE_VALUE;
  HANDLE reader_thread_handle_ = INVALID_HANDLE_VALUE;
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
