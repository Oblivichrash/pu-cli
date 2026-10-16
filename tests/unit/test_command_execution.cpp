// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace pu::platform;

TEST_CASE("ExecuteCommand runs a simple command", "[platform][command]") {
  std::string output;
  int rc = ExecuteCommand("echo hello", output);
  REQUIRE(rc == 0);
  REQUIRE(output.find("hello") != std::string::npos);
}

TEST_CASE("ExecuteCommand runs compound command with pipe", "[platform][command]") {
  std::string output;
#ifdef _WIN32
  int rc = ExecuteCommand("echo hello | findstr hello", output);
  REQUIRE(rc == 0);
  REQUIRE(output.find("hello") != std::string::npos);
#else
  int rc = ExecuteCommand("echo hello | tr a-z A-Z", output);
  REQUIRE(rc == 0);
  REQUIRE(output.find("HELLO") != std::string::npos);
#endif
}

#ifndef _WIN32
TEST_CASE("ExecuteCommand runs compound command with semicolons and redirection",
          "[platform][command]") {
  std::string output;
  int rc = ExecuteCommand("which g++ 2>&1; ls /usr/bin/g++ 2>&1", output);
  REQUIRE(rc == 0);
  REQUIRE(!output.empty());
  REQUIRE(output.find("g++") != std::string::npos);
}
#endif

TEST_CASE("ExecuteCommand reports non-zero exit code for failing command", "[platform][command]") {
  std::string output;
  int rc = ExecuteCommand("false", output);
  REQUIRE(rc != 0);
}

TEST_CASE("ExecuteCommand captures stderr via 2>&1 redirection", "[platform][command]") {
  std::string output;
  int rc = ExecuteCommand("ls /nonexistent_path_pu_test 2>&1", output);
  REQUIRE(rc != 0);
  REQUIRE(!output.empty());
}

TEST_CASE("A command that never returns is killed by its stop token", "[platform][command]") {
  auto stop_token = std::make_shared<std::atomic<bool>>(false);
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop_token->store(true);
  });

  std::string output;
  const auto started = std::chrono::steady_clock::now();
#ifdef _WIN32
  const int rc = ExecuteCommand("ping -n 60 127.0.0.1 > nul", output, stop_token);
#else
  const int rc = ExecuteCommand("sleep 60", output, stop_token);
#endif
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
  stopper.join();

  REQUIRE(rc == -1);
  REQUIRE(elapsed_ms < 5000);
}

TEST_CASE("The shutdown token is shared and cleared with the interrupt flag",
          "[platform][signal]") {
  ClearInterruptFlag();
  REQUIRE_FALSE(ShutdownToken()->load());

  ShutdownToken()->store(true);
  REQUIRE(ShutdownToken()->load());

  ClearInterruptFlag();
  REQUIRE_FALSE(ShutdownToken()->load());
}
