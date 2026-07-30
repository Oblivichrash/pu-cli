// SPDX-License-Identifier: GPL-3.0-only
#include "pu/mcp/stdio_transport.hpp"
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <csignal>

namespace pu::mcp {

StdioTransport::StdioTransport(const std::string& command, const std::vector<std::string>& args)
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
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return false;
    }

    if (pid_ == 0) { // Child process
        // Redirect stdin/stdout
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[1]); close(stdout_pipe[0]);
        close(stdin_pipe[0]); close(stdout_pipe[1]);

        // Build argv
        std::vector<const char*> exec_argv;
        exec_argv.push_back(command_.c_str());
        for (const auto& a : args_) exec_argv.push_back(a.c_str());
        exec_argv.push_back(nullptr);

        execvp(command_.c_str(), const_cast<char* const*>(exec_argv.data()));
        // If exec fails, log and exit
        spdlog::error("execvp failed: {}", strerror(errno));
        _exit(127);
    } else { // Parent process
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        stdin_fd_ = stdin_pipe[1];
        stdout_fd_ = stdout_pipe[0];
        return true;
    }
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
    if (stdin_fd_ != -1) { close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ != -1) { close(stdout_fd_); stdout_fd_ = -1; }
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
            std::string line = chunk.substr(0, pos);
            chunk.erase(0, pos + 1);
            if (!line.empty() && on_message_) {
                on_message_(line);
            }
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

} // namespace pu::mcp