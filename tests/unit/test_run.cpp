// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/platform.hpp"
#include "pu/exec/run.hpp"
#include "tests/mocks/test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace pu::exec;

namespace {

// A file of `count` known lines, so a page can be compared exactly.
std::filesystem::path LinesFile(const std::filesystem::path& dir, std::size_t count) {
  const auto path = dir / "lines.txt";
  std::ofstream out(path);
  for (std::size_t i = 1; i <= count; ++i) out << "line " << i << "\n";
  return path;
}

}  // namespace

TEST_CASE("Run runs a simple command", "[exec]") {
  const auto ran = Run("echo hello", pu::CancelToken{}, "", 0);
  REQUIRE(ran.verdict.exit_code == 0);
  REQUIRE(ran.verdict.ok);
  REQUIRE(ran.verdict.error.empty());
  REQUIRE(ran.captured.text.find("hello") != std::string::npos);
}

TEST_CASE("Run passes a compound command to the shell", "[exec]") {
#ifdef _WIN32
  const auto ran = Run("echo hello | findstr hello", pu::CancelToken{}, "", 0);
  REQUIRE(ran.verdict.exit_code == 0);
  REQUIRE(ran.captured.text.find("hello") != std::string::npos);
#else
  const auto ran = Run("echo hello | tr a-z A-Z", pu::CancelToken{}, "", 0);
  REQUIRE(ran.verdict.exit_code == 0);
  REQUIRE(ran.captured.text.find("HELLO") != std::string::npos);
#endif
}

#ifndef _WIN32
TEST_CASE("Run runs a compound command with semicolons and redirection", "[exec]") {
  const auto ran = Run("which g++ 2>&1; ls /usr/bin/g++ 2>&1", pu::CancelToken{}, "", 0);
  REQUIRE(ran.verdict.exit_code == 0);
  REQUIRE_FALSE(ran.captured.text.empty());
  REQUIRE(ran.captured.text.find("g++") != std::string::npos);
}
#endif

TEST_CASE("Run describes a failing command rather than only its code", "[exec]") {
  const auto ran = Run("false", pu::CancelToken{}, "", 0);
  REQUIRE_FALSE(ran.verdict.ok);
  REQUIRE(ran.verdict.exit_code != 0);
  REQUIRE(ran.verdict.error.find("Command failed") != std::string::npos);
}

TEST_CASE("Run merges stderr into the same stream", "[exec]") {
  const auto ran = Run("ls /nonexistent_path_pu_test 2>&1", pu::CancelToken{}, "", 0);
  REQUIRE_FALSE(ran.verdict.ok);
  REQUIRE_FALSE(ran.captured.text.empty());
}

TEST_CASE("A budget keeps only the bytes it allows", "[exec]") {
  pu::tests::ScopedTempDir dir("pu_capture");
  const auto file = LinesFile(dir.Path(), 200);

  const auto ran = Run(pu::tests::PrintFileCommand(file.string()), pu::CancelToken{}, "", 256);

  // The budget bounds what is kept, not what runs: the command still reached its own end.
  REQUIRE(ran.verdict.exit_code == 0);
  REQUIRE(ran.captured.extent.truncated());
  REQUIRE(ran.captured.extent.bytes > 256);
  REQUIRE(ran.captured.extent.lines == 200);
  REQUIRE(ran.captured.extent.retained < 200);
  REQUIRE(ran.captured.text.size() <= 256);
  // The cut landed between two lines, which is what makes a page readable.
  REQUIRE(ran.captured.text.back() == '\n');
  REQUIRE(ran.captured.text.rfind("line 1\n", 0) == 0);
}

TEST_CASE("A budget spent on the end keeps the newest lines", "[exec]") {
  pu::tests::ScopedTempDir dir("pu_capture_end");
  const auto file = LinesFile(dir.Path(), 200);

  const auto ran =
      Run(pu::tests::PrintFileCommand(file.string()), pu::CancelToken{}, "", 256, Keep::kEnd);

  REQUIRE(ran.verdict.exit_code == 0);
  REQUIRE(ran.captured.extent.truncated());
  REQUIRE(ran.captured.extent.lines == 200);
  REQUIRE(ran.captured.text.size() <= 256);
  // The newest lines are the ones kept, and they start on a boundary.
  REQUIRE(ran.captured.text.rfind("line 200\n") != std::string::npos);
  REQUIRE(ran.captured.text.front() != '\n');
  REQUIRE(ran.captured.extent.skipped() > 0);
  REQUIRE(ran.captured.extent.skipped() + ran.captured.extent.retained == 200);
}

TEST_CASE("No budget keeps everything", "[exec]") {
  pu::tests::ScopedTempDir dir("pu_capture_all");
  const auto file = LinesFile(dir.Path(), 200);

  const auto ran = Run(pu::tests::PrintFileCommand(file.string()), pu::CancelToken{}, "", 0);

  REQUIRE(ran.verdict.exit_code == 0);
  REQUIRE_FALSE(ran.captured.extent.truncated());
  REQUIRE(ran.captured.extent.lines == 200);
  REQUIRE(ran.captured.extent.retained == 200);
  REQUIRE(ran.captured.text.size() == ran.captured.extent.bytes);
}

// A reader that walks away has to leave the process dead behind it, and only the POSIX reader
// can: it waits in `poll` and kills the process group. The Windows reader is a blocking
// `popen` read and notices a token only between two reads, so on Windows a command that
// prints nothing runs to its own end. That gap is in Known Limitations, and this test is not
// written for a platform that cannot promise it.
#ifndef _WIN32
TEST_CASE("A command that keeps printing is stopped by its stop token", "[exec][stop]") {
  auto stop_token = std::make_shared<std::atomic<bool>>(false);
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop_token->store(true);
  });

  const auto started = std::chrono::steady_clock::now();
  const auto ran = Run("while true; do echo tick; sleep 0.1; done", stop_token, "", 0);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
  stopper.join();

  REQUIRE(ran.verdict.stopped);
  REQUIRE(ran.verdict.exit_code == -1);
  REQUIRE(elapsed_ms < 5000);
}

TEST_CASE("A silent command that never returns is killed too", "[exec][stop]") {
  // The reader blocks in `poll` rather than in a read, so a command that prints nothing still
  // sees the token — which is the case a blocking reader gets wrong.
  auto stop_token = std::make_shared<std::atomic<bool>>(false);
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop_token->store(true);
  });

  const auto started = std::chrono::steady_clock::now();
  const auto ran = Run("sleep 60", stop_token, "", 0);
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
  stopper.join();

  REQUIRE(ran.verdict.stopped);
  REQUIRE(elapsed_ms < 5000);
}
#endif

TEST_CASE("A shutdown request is a one-way trap the token and the flag agree on",
          "[platform][signal]") {
  // The token is process-wide: a test before this one may have left it set, and a test
  // after this one inherits whatever is left. Both ends are restored, so the flag's
  // one-way behavior is asserted inside that window alone.
  const bool was_set = pu::platform::ShutdownRequested();
  pu::platform::ShutdownToken()->store(false, std::memory_order_release);
  REQUIRE_FALSE(pu::platform::ShutdownRequested());

  pu::platform::ShutdownToken()->store(true, std::memory_order_release);

  REQUIRE(pu::platform::ShutdownRequested());
  REQUIRE(pu::platform::ShutdownToken()->load());
  pu::platform::ShutdownToken()->store(was_set, std::memory_order_release);
}
