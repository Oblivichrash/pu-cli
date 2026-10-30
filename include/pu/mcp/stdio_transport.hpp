// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <memory>
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

#ifdef _WIN32
  // The reader thread polls a handle and clears a flag; the transport must keep
  // both alive while it runs. The reader holds no ownership, so the transport
  // frees the context once the thread has been waited for.
  struct ReaderContext {
    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    std::atomic<bool>* running = nullptr;
    MessageCallback on_message;
  };
#endif

 private:
  bool SpawnProcess();
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
  std::unique_ptr<ReaderContext> reader_context_;
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
