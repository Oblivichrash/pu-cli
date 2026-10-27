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

TEST_CASE("PU_HOME moves the log directory and nothing else", "[logging]") {
  const auto scratch = std::filesystem::temp_directory_path() / "pu_log_dir_test";
  std::filesystem::remove_all(scratch);

  REQUIRE(pu::LogDir() == std::filesystem::current_path() / ".pu" / "logs");

  {
    ScopedEnvVar home("PU_HOME", scratch.string());
    REQUIRE(pu::LogDir() == scratch / "logs");
  }

  REQUIRE(pu::LogDir() == std::filesystem::current_path() / ".pu" / "logs");
  std::filesystem::remove_all(scratch);
}

// InitLogging runs again for every Runtime in the process, so every async writer it builds has
// to share one pool. A logger holds its pool weakly: replacing the pool under a logger leaves
// it posting to a queue that no longer exists. That is the shape of the crash a serve test hit
// when a second Runtime started in the same process.
TEST_CASE("InitLogging keeps the thread pool the loggers already use", "[logging]") {
  const auto scratch = std::filesystem::temp_directory_path() / "pu_log_pool_test";
  std::filesystem::remove_all(scratch);
  ScopedEnvVar home("PU_HOME", scratch.string());

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
  REQUIRE(std::filesystem::exists(scratch / "logs" / "pu.log"));
}

