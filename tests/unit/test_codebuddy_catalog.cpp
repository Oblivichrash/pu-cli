// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pu/llm/model_listing.hpp"

using namespace pu;

TEST_CASE("[codebuddy] the catalogue is read on the chat base's origin, not under it",
          "[codebuddy]") {
  REQUIRE(llm::CodeBuddyCatalogUrl("https://copilot.tencent.com/v2") ==
          "https://copilot.tencent.com/v3/config");
}

TEST_CASE("[codebuddy] a base carrying no version segment keeps its path",
          "[codebuddy]") {
  REQUIRE(llm::CodeBuddyCatalogUrl("https://copilot.example.com") ==
          "https://copilot.example.com/v3/config");
}

TEST_CASE("[codebuddy] only the trailing v2 segment is replaced", "[codebuddy]") {
  REQUIRE(llm::CodeBuddyCatalogUrl("https://proxy.example.com/copilot/v2") ==
          "https://proxy.example.com/copilot/v3/config");
}
