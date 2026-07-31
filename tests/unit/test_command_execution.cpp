// SPDX-License-Identifier: GPL-3.0-only
#include "pu/infra/platform.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace pu::platform;

TEST_CASE("ExecuteCommand runs a simple command", "[platform][command]") {
  std::string output;
  int rc = ExecuteCommand("echo hello", output);
  REQUIRE(rc == 0);
  REQUIRE(output.find("hello") != std::string::npos);
}

TEST_CASE("ExecuteCommand runs compound command with pipe", "[platform][command]") {
  std::string output;
  int rc = ExecuteCommand("echo hello | tr a-z A-Z", output);
  REQUIRE(rc == 0);
  REQUIRE(output.find("HELLO") != std::string::npos);
}

TEST_CASE("ExecuteCommand runs compound command with semicolons and redirection",
          "[platform][command]") {
  std::string output;
  // Exact acceptance command from DEEPSEEK.md — must not block and must return output.
  int rc = ExecuteCommand("which g++ 2>&1; ls /usr/bin/g++ 2>&1", output);
  REQUIRE(rc == 0);
  REQUIRE(!output.empty());
  REQUIRE(output.find("g++") != std::string::npos);
}

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
