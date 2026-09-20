// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/core/platform.hpp"
#include "pu/core/text.hpp"
#include "pu/tools/tool_result.hpp"

#include <boost/json.hpp>

#include <string>

using namespace pu;

TEST_CASE("IsValidUtf8 accepts ASCII and well-formed sequences", "[text]") {
  REQUIRE(text::IsValidUtf8(""));
  REQUIRE(text::IsValidUtf8("plain ascii"));
  REQUIRE(text::IsValidUtf8("\xE4\xB8\xAD"));           // U+4E2D, 3 bytes
  REQUIRE(text::IsValidUtf8("\xF0\x9F\x98\x80"));       // U+1F600, 4 bytes
  REQUIRE(text::IsValidUtf8("mixed \xE4\xB8\xAD ascii"));
}

TEST_CASE("IsValidUtf8 rejects malformed sequences", "[text]") {
  REQUIRE_FALSE(text::IsValidUtf8("\xB2\xBB"));         // GBK "not", illegal lead
  REQUIRE_FALSE(text::IsValidUtf8("\xE4\xB8"));         // truncated 3-byte
  REQUIRE_FALSE(text::IsValidUtf8("\xC0\x80"));         // overlong NUL
  REQUIRE_FALSE(text::IsValidUtf8("\xED\xA0\x80"));     // surrogate U+D800
  REQUIRE_FALSE(text::IsValidUtf8("\xF5\x80\x80\x80")); // above U+10FFFF
  REQUIRE_FALSE(text::IsValidUtf8("\xFF"));
  REQUIRE_FALSE(text::IsValidUtf8("ok \xB2\xBB ok"));
}

TEST_CASE("SanitizeUtf8 repairs malformed input and keeps valid text", "[text]") {
  const std::string gbk_text = "error: \xB2\xBB\xCA\xC7 command";
  REQUIRE_FALSE(text::IsValidUtf8(gbk_text));

  const std::string repaired = text::SanitizeUtf8(gbk_text);
  REQUIRE(text::IsValidUtf8(repaired));
  REQUIRE(repaired.find("error: ") == 0);
  REQUIRE(repaired.find(" command") != std::string::npos);
  REQUIRE(repaired.find(text::kUtf8ReplacementCharacter) != std::string::npos);
}

TEST_CASE("SanitizeUtf8 leaves valid input untouched", "[text]") {
  const std::string valid = "ascii \xE4\xB8\xAD\xE6\x96\x87 tail";
  REQUIRE(text::SanitizeUtf8(valid) == valid);
  REQUIRE(text::SanitizeUtf8("") == "");
}

TEST_CASE("Tool results parse back when the output is not UTF-8",
          "[tools][text]") {
  // GBK bytes for a localized "not found" message.
  const std::string gbk_stdout = "\xB2\xBB\xCA\xC7\xC4\xDA\xB2\xBF\xBB\xF2";

  const std::string result = tools::MakeToolResultJson(
      false, gbk_stdout, gbk_stdout, "Command failed (exit 1)", 1);

  const auto j = boost::json::parse(result);
  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("exit_code") == 1);
  REQUIRE(text::IsValidUtf8(boost::json::value_to<std::string>(j.at("stdout"))));
  REQUIRE(text::IsValidUtf8(boost::json::value_to<std::string>(j.at("stderr"))));

  const auto parsed = tools::ParseToolResult(result);
  REQUIRE(parsed.valid);
  REQUIRE_FALSE(parsed.success);
  REQUIRE(parsed.exit_code == 1);
}

TEST_CASE("Tool results keep valid text byte for byte", "[tools][text]") {
  const std::string utf8_stdout = "success \xE4\xB8\xAD\xE6\x96\x87";
  const std::string result =
      tools::MakeToolResultJson(true, utf8_stdout, "", "", 0);

  const auto j = boost::json::parse(result);
  REQUIRE(boost::json::value_to<std::string>(j.at("stdout")) == utf8_stdout);
}

TEST_CASE("ExecuteCommand returns valid UTF-8 for localized shell output",
          "[platform][text]") {
  // A localized shell message is not UTF-8 until the capture path decodes it.
  std::string output;
  pu::platform::ExecuteCommand("nonexistent_command_pu_encoding_test", output);

  REQUIRE_FALSE(output.empty());
  REQUIRE(text::IsValidUtf8(output));
}
