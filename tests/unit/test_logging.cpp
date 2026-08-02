// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/logging.hpp"
#include <nlohmann/json.hpp>

using namespace pu;

namespace {

std::string FormatRecord(const std::string& level) {
  JsonLogFormatter fmt;
  spdlog::details::log_msg msg{spdlog::source_loc{}, "pu",
                               spdlog::level::from_str(level), "hello world"};
  spdlog::memory_buf_t buf;
  fmt.format(msg, buf);
  return std::string(buf.data(), buf.size());
}

}  // namespace

TEST_CASE("JsonLogFormatter emits structured fields", "[logging]") {
  SetLogSessionId("sess-1");
  BeginRequest();
  SetLogToolName("execute_bash");
  SetLogDurationMs(42);

  auto j = nlohmann::json::parse(FormatRecord("info"));
  REQUIRE(j["level"] == "info");
  REQUIRE(j["session_id"] == "sess-1");
  REQUIRE(j["request_id"].is_string());
  REQUIRE(j["request_id"].get<std::string>().size() == 36);
  REQUIRE(j["tool_name"] == "execute_bash");
  REQUIRE(j["duration_ms"] == 42);
  REQUIRE(j["message"] == "hello world");
  REQUIRE(j.contains("timestamp"));

  ClearLogSessionId();
  ClearLogRequestId();
  ClearLogToolName();
  ClearLogDurationMs();
}

TEST_CASE("JsonLogFormatter omits unset optional fields", "[logging]") {
  ClearLogSessionId();
  ClearLogRequestId();
  ClearLogToolName();
  ClearLogDurationMs();

  auto j = nlohmann::json::parse(FormatRecord("warn"));
  REQUIRE(j["level"] == "warning");
  REQUIRE(j["message"] == "hello world");
  REQUIRE(!j.contains("session_id"));
  REQUIRE(!j.contains("request_id"));
  REQUIRE(!j.contains("tool_name"));
  REQUIRE(!j.contains("duration_ms"));
}

TEST_CASE("BeginRequest generates a valid UUID v4", "[logging]") {
  SetLogSessionId("sess");
  BeginRequest();

  auto j = nlohmann::json::parse(FormatRecord("info"));
  std::string rid = j["request_id"].get<std::string>();
  REQUIRE(rid.size() == 36);
  REQUIRE(rid[8] == '-');
  REQUIRE(rid[13] == '-');
  REQUIRE(rid[18] == '-');
  REQUIRE(rid[23] == '-');
  REQUIRE(rid[14] == '4');  // version nibble

  ClearLogSessionId();
  ClearLogRequestId();
}

TEST_CASE("JsonLogFormatter escapes quotes and newlines", "[logging]") {
  ClearLogSessionId();
  ClearLogRequestId();
  ClearLogToolName();
  ClearLogDurationMs();

  JsonLogFormatter fmt;
  spdlog::details::log_msg msg{spdlog::source_loc{}, "pu",
                               spdlog::level::info, "say \"hi\"\nnext line"};
  spdlog::memory_buf_t buf;
  fmt.format(msg, buf);
  auto j = nlohmann::json::parse(std::string(buf.data(), buf.size()));
  REQUIRE(j["message"] == "say \"hi\"\nnext line");
}
