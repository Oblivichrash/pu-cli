// SPDX-License-Identifier: GPL-3.0-only

#include "backends/streaming_json_parser.hpp"
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <string>

using namespace pu::backends;

TEST_CASE("StreamingJsonParser splits complete lines", "[streaming_parser]") {
  std::vector<std::string> lines;
  auto parser = StreamingJsonParser(
    [&](std::string_view line) { lines.emplace_back(line); },
    [](const std::string&) {}
  );

  const char* input = "line1\nline2\nline3\n";
  parser.Feed(input, 18);
  REQUIRE(lines.size() == 3);
  REQUIRE(lines[0] == "line1");
  REQUIRE(lines[1] == "line2");
  REQUIRE(lines[2] == "line3");
}

TEST_CASE("StreamingJsonParser handles \\r\\n", "[streaming_parser]") {
  std::vector<std::string> lines;
  auto parser = StreamingJsonParser(
    [&](std::string_view line) { lines.emplace_back(line); },
    [](const std::string&) {}
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
    [](const std::string&) {}
  );

  const char* part1 = "{\"val\":\"\xE2\x82";
  const char* part2 = "\xAC\"}\n";

  parser.Feed(part1, 10);
  REQUIRE(lines.empty());  // still incomplete

  parser.Feed(part2, 5);
  REQUIRE(lines.size() == 1);
  REQUIRE(lines[0] == "{\"val\":\"€\"}");
}

TEST_CASE("StreamingJsonParser skips empty lines", "[streaming_parser]") {
  std::vector<std::string> lines;
  auto parser = StreamingJsonParser(
    [&](std::string_view line) { lines.emplace_back(line); },
    [](const std::string&) {}
  );

  parser.Feed("\n\nhello\n\n", 9);
  REQUIRE(lines.size() == 1);
  REQUIRE(lines[0] == "hello");
}
