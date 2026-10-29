// SPDX-License-Identifier: GPL-3.0-only

//
// What `pu check` says about a file, read back from the text it prints.
//
// The command exists to explain a config, so its output is the thing under test: the resolved
// host a gateway supplied, the length an `${ENV}` reference expanded to, the directory a
// relative sandbox resolves against, and the fields this build never reads.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <cstdio>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <iostream>
#include <string>

#include "pu/app/cli.hpp"
#include "pu/session/session.hpp"

namespace {

namespace fs = std::filesystem;

// `pu check` writes to stdout, so the harness captures it while the command runs.
class CapturedStdout {
 public:
  CapturedStdout() { old_ = std::cout.rdbuf(buffer_.rdbuf()); }
  ~CapturedStdout() { std::cout.rdbuf(old_); }
  std::string text() const { return buffer_.str(); }

 private:
  std::ostringstream buffer_;
  std::streambuf* old_ = nullptr;
};

class TempWorkspace {
 public:
  TempWorkspace() {
    static int counter = 0;
    static std::mt19937 engine{std::random_device{}()};
    root_ = fs::temp_directory_path() /
            ("pu_check_" + std::to_string(engine()) + "_" + std::to_string(counter++));
    fs::create_directories(root_ / ".pu");
  }
  ~TempWorkspace() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  fs::path Write(const std::string& text) const {
    const fs::path path = root_ / ".pu" / "agents.json";
    std::ofstream(path) << text;
    return path;
  }

  const fs::path& root() const { return root_; }

 private:
  fs::path root_;
};

int RunCheckOf(const fs::path& config) {
  CapturedStdout capture;
  return pu::cli::RunCheck(config.string());
}

}  // namespace

TEST_CASE("A gateway's host is printed as the value that will be used", "[check]") {
  TempWorkspace workspace;
  const fs::path config = workspace.Write(
      R"({"default_agent":"a","agents":[{"name":"a","backend":{"gateway":"deepseek","model":"m"}}]})");

  CapturedStdout capture;
  REQUIRE(pu::cli::RunCheck(config.string()) == pu::cli::kOk);

  const std::string out = capture.text();
  // The file names no host, so the printed one came from the gateway table; saying where it
  // came from is the point of printing it.
  REQUIRE(out.find("gateway      deepseek") != std::string::npos);
  REQUIRE(out.find("(default for this gateway)") != std::string::npos);
}

TEST_CASE("A sandbox root is printed as the directory it resolves to", "[check]") {
  TempWorkspace workspace;
  const fs::path config = workspace.Write(
      R"({"default_agent":"a","agents":[{"name":"a","backend":{"gateway":"ollama","host":"http://h:1","model":"m"},"security":{"sandbox_root":"."}}]})");

  CapturedStdout capture;
  REQUIRE(pu::cli::RunCheck(config.string()) == pu::cli::kOk);

  // "." is relative to where the command runs, which is what a reader cannot see in the file.
  REQUIRE(capture.text().find("sandbox      " + fs::current_path().string()) !=
          std::string::npos);
}

TEST_CASE("An api key that expanded to nothing is called out", "[check]") {
  TempWorkspace workspace;
  // A variable this machine does not set: what a typo in ${...} produces.
  const fs::path config = workspace.Write(
      R"({"default_agent":"a","agents":[{"name":"a","backend":{"gateway":"ollama","host":"http://h:1","model":"m","api_key":"${PU_CHECK_UNSET_VARIABLE}"}}]})");

  CapturedStdout capture;
  REQUIRE(pu::cli::RunCheck(config.string()) == pu::cli::kOk);

  const std::string out = capture.text();
  REQUIRE(out.find("api_key      (empty)") != std::string::npos);
  // The warning names the reference, so the fix is "set this", not "your key is wrong".
  REQUIRE(out.find("PU_CHECK_UNSET_VARIABLE") != std::string::npos);
}

TEST_CASE("A field this build ignores is reported, and is not a failure", "[check]") {
  TempWorkspace workspace;
  const fs::path config = workspace.Write(
      R"({"default_agent":"a","agents":[{"name":"a","backend":{"gateway":"ollama","host":"http://h:1","model":"m"},"security":{"history_compaction":true},"mystery":1}]})");

  CapturedStdout capture;
  // A build may be older than the file, so an unread field is a warning: the config runs.
  REQUIRE(pu::cli::RunCheck(config.string()) == pu::cli::kOk);

  const std::string out = capture.text();
  REQUIRE(out.find("'mystery'") != std::string::npos);
  REQUIRE(out.find("'history_compaction'") != std::string::npos);
  REQUIRE(out.find("not read by this build") != std::string::npos);
}

TEST_CASE("A file that sets every field it can is called out for none of them", "[check]") {
  TempWorkspace workspace;
  // Every key ParseAgentEntry and the parsers under it read, written out. The point is not
  // that this config is realistic — it is that a key the parser reads and the check does not
  // know about turns a working setting into advice to delete it. `paging` was that key.
  const fs::path config = workspace.Write(R"({
    "default_agent": "a",
    "agents": [{
      "name": "a",
      "description": "d",
      "backend": {
        "gateway": "ollama", "host": "http://h:1", "model": "m", "api_key": "k",
        "temperature": 0.2, "system_prompt": "s", "max_tokens": 16,
        "thinking": "low", "context_window": 4096
      },
      "security": {
        "sandbox_root": ".", "max_command_length": 100,
        "forbidden_patterns": ["rm -rf"], "approval_patterns": ["git push"]
      },
      "paging": {
        "page_lines": 100, "max_lines": 200, "capture_bytes": 8192,
        "retain_bytes": 16384, "retain_buffers": 2
      },
      "mcp_servers": [{"name": "s", "command": "c"}],
      "tool_call_budget": 4
    }]
  })");

  CapturedStdout capture;
  REQUIRE(pu::cli::RunCheck(config.string()) == pu::cli::kOk);

  INFO(capture.text());
  REQUIRE(capture.text().find("not read by this build") == std::string::npos);
}

TEST_CASE("A misspelt paging field is reported, like any other unread field", "[check]") {
  TempWorkspace workspace;
  // The budget block is nested one level deeper than the rest, so it is the one a reader
  // writes a stray underscore into and hears nothing about.
  const fs::path config = workspace.Write(
      R"({"default_agent":"a","agents":[{"name":"a","backend":{"gateway":"ollama","host":"http://h:1","model":"m"},"paging":{"page_line":100}}]})");

  CapturedStdout capture;
  REQUIRE(pu::cli::RunCheck(config.string()) == pu::cli::kOk);

  const std::string out = capture.text();
  REQUIRE(out.find("'page_line' in a.paging is not read by this build") != std::string::npos);
}

TEST_CASE("A file this build cannot load exits 1", "[check]") {
  TempWorkspace workspace;
  // The 0.5.0 rename: `type` is not read, and the message says which key to write instead.
  const fs::path config = workspace.Write(
      R"({"default_agent":"a","agents":[{"name":"a","backend":{"type":"ollama","model":"m"}}]})");

  REQUIRE(RunCheckOf(config) == pu::cli::kUsage);
}

TEST_CASE("A session naming an agent the file no longer has is reported", "[check]") {
  TempWorkspace workspace;
  const fs::path config = workspace.Write(
      R"({"default_agent":"a","agents":[{"name":"a","backend":{"gateway":"ollama","host":"http://h:1","model":"m"}}]})");
  std::ofstream(workspace.root() / ".pu" / "session.json")
      << R"({"schema_version":)" << pu::kSessionSchemaVersion
      << R"(,"conversation":{"history":{"nodes":[],"leaf":""}},"session_spec":{"agent_name":"gone"}})";

  CapturedStdout capture;
  // The session outranks default_agent by design, so a stale name is a fact to print, not a
  // file to refuse: the exit code stays 0 and the names that do exist are listed.
  REQUIRE(pu::cli::RunCheck(config.string()) == pu::cli::kOk);

  const std::string out = capture.text();
  REQUIRE(out.find("names  gone, which this file does not configure") != std::string::npos);
  REQUIRE(out.find("configured: a") != std::string::npos);
}
