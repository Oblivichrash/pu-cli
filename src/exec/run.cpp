// SPDX-License-Identifier: GPL-3.0-only

// Running a command, and the budget its output is read under. The reader owns the
// verdict as well as the bytes: it is the only party that knows whether the process it
// was reading finished or was killed, and an exit code means nothing without that.

#include "pu/exec/run.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"
#include "pu/session/message.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pu::exec {

namespace {

std::size_t CountLines(std::string_view text) {
  if (text.empty()) return 0;
  std::size_t lines = 0;
  for (const char c : text) {
    if (c == '\n') ++lines;
  }
  return text.back() == '\n' ? lines : lines + 1;
}

// Keeps the end of the stream the caller asked for and counts the rest. Trimming waits
// for the end of the stream, so a byte budget never leaves half a UTF-8 sequence or half
// a line at the cut.
class Capture {
 public:
  Capture(std::size_t max_bytes, Keep keep) : max_bytes_(max_bytes), keep_(keep) {}

  void Append(const char* data, std::size_t size) {
    if (size == 0) return;
    extent_.bytes += size;
    for (std::size_t i = 0; i < size; ++i) {
      if (data[i] == '\n') ++newlines_;
    }
    last_ = data[size - 1];

    if (max_bytes_ == 0) {
      text_.append(data, size);
      return;
    }
    if (keep_ == Keep::kHead) {
      const std::size_t room = text_.size() < max_bytes_ ? max_bytes_ - text_.size() : 0;
      const std::size_t kept = std::min(size, room);
      if (kept > 0) text_.append(data, kept);
      if (kept < size) truncated_ = true;
      return;
    }
    text_.append(data, size);
    // Compacting a quarter late keeps the cost per byte read constant; the last quarter
    // is dropped in Take, so the buffer it hands over is within the budget.
    if (text_.size() * 4 > max_bytes_ * 5) DropToBudget();
    return;
  }

  Captured Take() {
    // A stream that did not end with a newline still ended a line.
    extent_.lines = newlines_ + (extent_.bytes > 0 && last_ != '\n' ? 1 : 0);
    extent_.from_end = keep_ == Keep::kEnd;

    if (keep_ == Keep::kHead && truncated_) {
      while (!text_.empty() && text::EndsWithPartialSequence(text_)) {
        text_.pop_back();
      }
      // The budget ran out mid-line, so that line is not a line this buffer has.
      const std::size_t newline = text_.rfind('\n');
      if (newline != std::string::npos) text_.resize(newline + 1);
    }
    if (keep_ == Keep::kEnd && truncated_) DropToBudget();

    extent_.retained = CountLines(text_);
    return Captured{std::move(text_), extent_};
  }

 private:
  // Keeps the newest whole lines, one line boundary at a time.
  void DropToBudget() {
    while (text_.size() > max_bytes_) {
      const std::size_t cut = text_.size() - max_bytes_;
      const std::size_t newline = text_.find('\n', cut);
      // A stream of one enormous line offers no boundary to cut on.
      const std::size_t drop = newline == std::string::npos ? cut : newline + 1;
      text_.erase(0, drop);
      truncated_ = true;
    }
  }

  Extent extent_;
  std::string text_;
  std::size_t max_bytes_;
  Keep keep_;
  std::size_t newlines_ = 0;
  char last_ = '\n';
  bool truncated_ = false;
};

void Settle(Verdict& verdict, bool stopped, int exit_code) {
  verdict.stopped = stopped;
  verdict.exit_code = exit_code;
  verdict.ok = exit_code == 0 && !stopped;
  if (stopped) {
    verdict.error = session::kCommandStoppedReceipt;
  } else if (exit_code != 0 && exit_code != -1) {
    verdict.error = "Command failed (exit " + std::to_string(exit_code) + ")";
  }
}

}  // namespace

#ifndef _WIN32

Ran Run(std::string_view command, const CancelToken& stop_token, std::string_view working_dir,
        std::size_t max_bytes, Keep keep) {
  Ran ran;
  ran.verdict.command = std::string(command);

  std::string full_cmd;
  if (!working_dir.empty()) {
    full_cmd = "cd \"" + std::filesystem::path(working_dir).string() + "\" && (" +
               std::string(command) + ")";
  } else {
    full_cmd = std::string(command);
  }
  // The merged stream is the tool's whole output; the caller cannot tell stderr from
  // stdout afterward, which is why the result carries one field for it.
  full_cmd += " 2>&1";

  auto start = std::chrono::steady_clock::now();
  const bool stops = static_cast<bool>(stop_token);

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    Settle(ran.verdict, false, -1);
    ran.verdict.error = "could not start the command: pipe failed";
    return ran;
  }

  // setpgid in the child detaches it from this process's group, so a kill of the group
  // takes the shell and any grandchildren with it.
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    Settle(ran.verdict, false, -1);
    ran.verdict.error = "could not start the command: fork failed";
    return ran;
  }
  if (pid == 0) {
    setpgid(0, 0);
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[1]);
    execl("/bin/sh", "sh", "-c", full_cmd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  close(pipe_fds[1]);
  setpgid(pid, pid);  // raced with the child's own call; idempotent either way

  // The stream is fdopen'd, not popen'd, so pclose is not an option: it answers for
  // popen's pid table only, and on this stream returns 0 regardless of the child. fclose
  // closes the descriptor and waitpid reaps the child explicitly.
  FILE* pipe_stream = fdopen(pipe_fds[0], "r");
  if (pipe_stream == nullptr) {
    close(pipe_fds[0]);
    Settle(ran.verdict, false, -1);
    ran.verdict.error = "could not read the command's output: fdopen failed";
    return ran;
  }

  // fgets parks on the pipe until the child writes, so a quiet command would keep the
  // token unchecked until it finished. The read runs on a poll tick instead: output when
  // there is any, the token when there is not.
  Capture capture(max_bytes, keep);
  for (;;) {
    struct pollfd pfd = {pipe_fds[0], POLLIN, 0};
    const int ready = poll(&pfd, 1, stops ? 50 : -1);
    if (ready > 0) {
      char buffer[4096];
      const ssize_t n = read(pipe_fds[0], buffer, sizeof buffer);
      if (n <= 0) break;
      capture.Append(buffer, static_cast<std::size_t>(n));
      if (stops && stop_token->load(std::memory_order_acquire)) break;
    } else if (stops && stop_token->load(std::memory_order_acquire)) {
      break;
    }
  }

  const bool stopped = stops && stop_token->load(std::memory_order_acquire);
  if (stopped) kill(-pid, SIGKILL);  // the group, so grandchildren go too

  fclose(pipe_stream);
  int status = 0;
  waitpid(pid, &status, 0);

  int exit_code = -1;
  if (!stopped && status != -1 && WIFEXITED(status)) exit_code = WEXITSTATUS(status);

  ran.captured = capture.Take();
  Settle(ran.verdict, stopped, exit_code);

  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  // The command and its byte count are the whole story at debug level; the output itself
  // reaches the transcript, and a full copy here grows the file sink alone.
  spdlog::debug(
      "[exec] command='{}' exit={} elapsed_ms={} printed={}B kept={}B lines={}/{} stopped={}",
      command, exit_code, elapsed_ms, ran.captured.extent.bytes, ran.captured.text.size(),
      ran.captured.extent.retained, ran.captured.extent.lines, stopped);
  return ran;
}

#else  // _WIN32

Ran Run(std::string_view command, const CancelToken& stop_token, std::string_view working_dir,
        std::size_t max_bytes, Keep keep) {
  Ran ran;
  ran.verdict.command = std::string(command);

  // Windows keeps the popen path; a stop token is honored only between reads of the
  // merged stream, which is no worse than what the stream reader does with cancel.
  std::string full_cmd;
  if (!working_dir.empty()) {
    full_cmd = "cd /d \"" + std::filesystem::path(working_dir).string() + "\" && (" +
               std::string(command) + ")";
  } else {
    full_cmd = std::string(command);
  }
  full_cmd += " 2>&1";

  auto start = std::chrono::steady_clock::now();

  FILE* pipe_stream = _popen(full_cmd.c_str(), "r");
  if (pipe_stream == nullptr) {
    Settle(ran.verdict, false, -1);
    ran.verdict.error = "could not start the command: popen failed";
    return ran;
  }

  Capture capture(max_bytes, keep);
  std::array<char, 4096> buffer{};
  bool stopped = false;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe_stream) != nullptr) {
    capture.Append(buffer.data(), std::strlen(buffer.data()));
    if (stop_token && stop_token->load(std::memory_order_acquire)) {
      stopped = true;
      break;
    }
  }

  ran.captured = capture.Take();
  ran.captured.text = platform::FromConsoleOutput(ran.captured.text);

  const int status = _pclose(pipe_stream);
  Settle(ran.verdict, stopped, status);

  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  spdlog::debug(
      "[exec] command='{}' exit={} elapsed_ms={} printed={}B kept={}B lines={}/{} stopped={}",
      command, status, elapsed_ms, ran.captured.extent.bytes, ran.captured.text.size(),
      ran.captured.extent.retained, ran.captured.extent.lines, stopped);
  return ran;
}

#endif

}  // namespace pu::exec
