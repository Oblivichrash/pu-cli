// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/logging.hpp"
#include <boost/json.hpp>
#include "pu/json.hpp"

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
  spdlog::details::log_msg msg{spdlog::source_loc{}, "pu",
                               spdlog::level::info, "say \"hi\"\nnext line"};
  spdlog::memory_buf_t buf;
  fmt.format(msg, buf);
  auto j = boost::json::parse(std::string(buf.data(), buf.size()));
  REQUIRE(j.at("message") == "say \"hi\"\nnext line");
}