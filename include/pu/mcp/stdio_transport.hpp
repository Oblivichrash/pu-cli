// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <sys/types.h>

namespace pu::mcp {

using MessageCallback = std::function<void(const std::string&)>;

class StdioTransport {
public:
    StdioTransport(const std::string& command, const std::vector<std::string>& args);
    ~StdioTransport();

    // Non-copyable
    StdioTransport(const StdioTransport&) = delete;
    StdioTransport& operator=(const StdioTransport&) = delete;

    // Start child process and reader thread
    bool Start(MessageCallback on_message);
    // Stop child process and reader thread
    void Stop();
    // Write a line to stdin (newline appended automatically)
    bool WriteLine(const std::string& line);

    bool IsRunning() const { return running_; }

private:
    bool SpawnProcess();
    void ReaderLoop();

    std::string command_;
    std::vector<std::string> args_;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    pid_t pid_ = -1;
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
    MessageCallback on_message_;
};

} // namespace pu::mcp