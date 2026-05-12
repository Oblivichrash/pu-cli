// SPDX-License-Identifier: GPL-3.0-only

#include "backends/streaming_json_parser.hpp"
#include "pu/error_codes.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>

using namespace pu::backends;

TEST_CASE("StreamingJsonParser splits complete lines", "[streaming_parser]") {
  std::vector<std::string> lines;
  auto parser = StreamingJsonParser(
    [&](std::string_view line) { lines.emplace_back(line); },
    [](std::error_code) {}
  );

  parser.Feed("line1\nline2\nline3\n", 17);
  REQUIRE(lines.size() == 3);
  REQUIRE(lines[0] == "line1");
  REQUIRE(lines[1] == "line2");
  REQUIRE(lines[2] == "line3");
}

TEST_CASE("StreamingJsonParser handles \\r\\n", "[streaming_parser]") {
  std::vector<std::string> lines;
  auto parser = StreamingJsonParser(
    [&](std::string_view line) { lines.emplace_back(line); },
    [](std::error_code) {}
  );

  parser.Feed("hello\r\nworld\r\n", 14);
  REQUIRE(lines.size() == 2);
  REQUIRE(lines[0] == "hello");
  REQUIRE(lines[1] == "world");
}

TEST_CASE("StreamingJsonParser handles fragmented UTF-8", "[streaming_parser]") {
  std::vector<std::string> lines;
  auto parser = StreamingJsonParser(
    [&](std::string_view line) { lines.emplace_back(line); },
    [](std::error_code) {}
  );

  // "你好" encoded as E4 BD A0 E5 A5 BD
  const char* part1 = "{\"msg\":\"\xE4\xBD\xA0";   // partial UTF-8 (missing last byte)
  const char* part2 = "\xE5\xA5\xBD\"}\n";

  parser.Feed(part1, 10);
  REQUIRE(lines.empty());  // line held back

  parser.Feed(part2, 6);
  REQUIRE(lines.size() == 1);
  REQUIRE(lines[0] == "{\"msg\":\"你好\"}");
}

TEST_CASE("StreamingJsonParser skips empty lines", "[streaming_parser]") {
  std::vector<std::string> lines;
  auto parser = StreamingJsonParser(
    [&](std::string_view line) { lines.emplace_back(line); },
    [](std::error_code) {}
  );

  parser.Feed("\n\nhello\n\n", 9);
  REQUIRE(lines.size() == 1);
  REQUIRE(lines[0] == "hello");
}
