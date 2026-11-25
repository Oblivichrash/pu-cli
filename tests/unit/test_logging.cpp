// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/logging.hpp"
#include "pu/core/base.hpp"
#include "tests/mocks/test_helpers.hpp"
#include <boost/json.hpp>
#include "pu/core/json.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>

#include <atomic>
#include <thread>

using namespace pu;
using pu::tests::ScopedEnvVar;
using pu::tests::ScopedTempDir;
using pu::tests::ScopedWorkspaceRoot;

namespace {

std::string FormatRecord(const std::string& level) {
  JsonLogFormatter fmt;
  spdlog::details::log_msg msg{spdlog::source_loc{}, "pu", spdlog::level::from_str(level),
                               "hello world"};
  spdlog::memory_buf_t buf;
  fmt.format(msg, buf);
  return std::string(buf.data(), buf.size());
}

}  // namespace

TEST_CASE("JsonLogFormatter emits structured fields", "[logging]") {
  BeginRequest();
  SetLogToolName("execute_bash");
  SetLogDurationMs(42);

  auto j = boost::json::parse(FormatRecord("info"));
  REQUIRE(j.at("level") == "info");
  REQUIRE(j.at("request_id").is_string());
  REQUIRE(boost::json::value_to<std::string>(j.at("request_id")).size() == 36);
  REQUIRE(j.at("tool_name") == "execute_bash");
  REQUIRE(j.at("duration_ms") == 42);
  REQUIRE(j.at("message") == "hello world");
  REQUIRE(json::HasKey(j, "timestamp"));

  ClearLogRequestId();
  ClearLogToolName();
  ClearLogDurationMs();
}

TEST_CASE("JsonLogFormatter omits unset optional fields", "[logging]") {
  ClearLogRequestId();
  ClearLogToolName();
  ClearLogDurationMs();

  auto j = boost::json::parse(FormatRecord("warn"));
  REQUIRE(j.at("level") == "warning");
  REQUIRE(j.at("message") == "hello world");
  REQUIRE(!json::HasKey(j, "request_id"));
  REQUIRE(!json::HasKey(j, "tool_name"));
  REQUIRE(!json::HasKey(j, "duration_ms"));
}

TEST_CASE("BeginRequest generates a valid UUID v4", "[logging]") {
  BeginRequest();

  auto j = boost::json::parse(FormatRecord("info"));
  std::string rid = boost::json::value_to<std::string>(j.at("request_id"));
  REQUIRE(rid.size() == 36);
  REQUIRE(rid[8] == '-');
  REQUIRE(rid[13] == '-');
  REQUIRE(rid[18] == '-');
  REQUIRE(rid[23] == '-');
  REQUIRE(rid[14] == '4');  // version nibble

  ClearLogRequestId();
}

TEST_CASE("JsonLogFormatter escapes quotes and newlines", "[logging]") {
  ClearLogRequestId();
  ClearLogToolName();
  ClearLogDurationMs();

  JsonLogFormatter fmt;
  spdlog::details::log_msg msg{spdlog::source_loc{}, "pu", spdlog::level::info,
                               "say \"hi\"\nnext line"};
  spdlog::memory_buf_t buf;
  fmt.format(msg, buf);
  auto j = boost::json::parse(std::string(buf.data(), buf.size()));
  REQUIRE(j.at("message") == "say \"hi\"\nnext line");
}

TEST_CASE("PU_HOME moves the log directory and nothing else", "[logging][process-state]") {
  // A name of its own per process: catch_discover_tests runs cases in parallel, and a fixed
  // name is a directory two of them write to at once.
  const ScopedTempDir scratch("pu_log_dir_test_");
  // Pinned empty, so this case reads the fallback it is about and not whatever root another
  // case in this process left behind.
  ScopedWorkspaceRoot no_root({});

  // Compared against the root, not against `current_path()`: another case in this process may
  // have moved the working directory under us, and the answer without a root is defined as
  // "the directory this process is in now" rather than the one it started in.
  REQUIRE(pu::LogDir() == pu::WorkspaceRoot() / ".pu" / "logs");

  {
    ScopedEnvVar home("PU_HOME", scratch.Path().string());
    REQUIRE(pu::LogDir() == scratch.Path() / "logs");
  }

  REQUIRE(pu::LogDir() == pu::WorkspaceRoot() / ".pu" / "logs");
}

// A run started with `-C` is about that directory, so its log has to be written there and not
// where the shell happened to be. The working directory is deliberately left alone, since that
// is the whole thing the root is meant to override.
TEST_CASE("LogDir follows the workspace root rather than the working directory",
          "[logging][process-state]") {
  const ScopedTempDir workspace("pu_log_ws_test_");
  ScopedWorkspaceRoot root(workspace.Path());

  REQUIRE(pu::LogDir() == workspace.Path() / ".pu" / "logs");

  // An unset root is not an empty one: it means nobody said, and the answer goes back to the
  // directory the process is in.
  pu::SetWorkspaceRoot({});
  REQUIRE(pu::LogDir() == pu::WorkspaceRoot() / ".pu" / "logs");
}

// PU_HOME is a statement about the machine, the root about this run, and the machine wins:
// a reader who set it asked for one directory for every project they work in.
TEST_CASE("PU_HOME outranks the workspace root for logs", "[logging][process-state]") {
  const ScopedTempDir workspace("pu_log_ws_home_test_");
  const ScopedTempDir home("pu_log_ws_home_env_");
  ScopedWorkspaceRoot root(workspace.Path());

  ScopedEnvVar env("PU_HOME", home.Path().string());
  REQUIRE(pu::LogDir() == home.Path() / "logs");
}

// InitLogging runs again for every Runtime in the process, so every async writer it builds has
// to share one pool. A logger holds its pool weakly: replacing the pool under a logger leaves
// it posting to a queue that no longer exists. That is the shape of the crash a serve test hit
// when a second Runtime started in the same process.
TEST_CASE("InitLogging keeps the thread pool the loggers already use", "[logging]") {
  const ScopedTempDir scratch("pu_log_pool_test_");
  ScopedEnvVar home("PU_HOME", scratch.Path().string());

  InitLogging("info");
  const auto first_pool = spdlog::thread_pool();
  REQUIRE(first_pool != nullptr);

  std::atomic<bool> stop{false};
  std::thread writer([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      spdlog::info("a message written while another logger is built");
    }
  });
  for (int i = 0; i < 8; ++i) InitLogging("info");
  stop.store(true, std::memory_order_relaxed);
  writer.join();

  INFO("a later logger must not replace the pool an earlier logger still writes through");
  REQUIRE(spdlog::thread_pool().get() == first_pool.get());
  REQUIRE(std::filesystem::exists(scratch.Path() / "logs" / "pu.log"));
}

// The bug this guards: `IsLoggingInitialized` used to read `spdlog::default_logger_raw()`, and
// spdlog installs a logger before `main` runs. The predicate therefore answered yes in a process
// that had never configured a destination, `Runtime::Initialize` skipped `InitLogging` on that
// answer, and a run left no `pu.log` behind while every call appeared to be logging.
//
// Only one direction is asserted: once anything in this process has initialised, the answer must
// be true -- including after the registry's own logger is replaced, which is what the pointer
// could not tell apart from a configured one.
TEST_CASE("IsLoggingInitialized reports a destination this process installed",
          "[logging][process-state]") {
  InitLogging("info");
  REQUIRE(IsLoggingInitialized());

  auto saved = spdlog::default_logger();
  auto& registry = spdlog::details::registry::instance();
  const ScopedTempDir scratch("pu_log_init_test_");
  {
    ScopedEnvVar home("PU_HOME", scratch.Path().string());
    registry.set_default_logger(nullptr);
    INFO("swapping the registry's logger does not un-initialise this process");
    REQUIRE(IsLoggingInitialized());
  }
  registry.set_default_logger(saved);
}
