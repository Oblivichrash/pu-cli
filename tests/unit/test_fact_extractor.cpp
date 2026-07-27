// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/fact_extractor.hpp"
#include "pu/session/workspace.hpp"

using namespace pu;

TEST_CASE("FactExtractor extracts file paths from messages", "[fact_extractor]") {
  auto ctx = std::make_shared<Workspace>("test");

  ctx->Append("user", "Check /home/user/project/main.cpp for issues");
  ctx->Append("assistant", "Looking at src/utils/helper.h");
  ctx->Append("user", "Also check README.md");

  FactExtractor extractor;
  auto facts = extractor.Extract(ctx, "testing");

  REQUIRE(facts.size() >= 2);  // At least main.cpp and helper.h

  bool found_main = false;
  bool found_helper = false;
  for (const auto& f : facts) {
    if (f.type == Artifact::Type::kFilePath) {
      if (f.content.find("main.cpp") != std::string::npos) found_main = true;
      if (f.content.find("helper.h") != std::string::npos) found_helper = true;
    }
  }

  REQUIRE(found_main);
  REQUIRE(found_helper);
}

TEST_CASE("FactExtractor extracts error messages", "[fact_extractor]") {
  auto ctx = std::make_shared<Workspace>("test");

  ctx->Append("user", "The build failed with an error");
  ctx->Append("assistant", "I found a compile error in the code");
  ctx->Append("user", "No issues here");

  FactExtractor extractor;
  auto facts = extractor.Extract(ctx, "testing");

  bool found_error = false;
  for (const auto& f : facts) {
    if (f.type == Artifact::Type::kErrorMsg) {
      found_error = true;
      break;
    }
  }

  REQUIRE(found_error);
}

TEST_CASE("FactExtractor deduplicates facts", "[fact_extractor]") {
  auto ctx = std::make_shared<Workspace>("test");

  ctx->Append("user", "Check /tmp/file.cpp");
  ctx->Append("assistant", "Check /tmp/file.cpp again");

  FactExtractor extractor;
  auto facts = extractor.Extract(ctx, "testing");

  // Count occurrences of /tmp/file.cpp
  int count = 0;
  for (const auto& f : facts) {
    if (f.content == "/tmp/file.cpp") count++;
  }

  REQUIRE(count == 1);  // Should be deduplicated
}

TEST_CASE("FactExtractor returns empty for empty context", "[fact_extractor]") {
  auto ctx = std::make_shared<Workspace>("test");

  FactExtractor extractor;
  auto facts = extractor.Extract(ctx, "testing");

  REQUIRE(facts.empty());
}

TEST_CASE("FactExtractor returns empty for null context", "[fact_extractor]") {
  FactExtractor extractor;
  auto facts = extractor.Extract(nullptr, "testing");

  REQUIRE(facts.empty());
}

TEST_CASE("FactExtractor handles mixed content", "[fact_extractor]") {
  auto ctx = std::make_shared<Workspace>("test");

  ctx->Append("user", "Found error in /var/log/syslog");
  ctx->Append("assistant", "The application crashed with a segmentation fault");
  ctx->Append("user", "Fixed in /home/user/fix.patch");

  FactExtractor extractor;
  auto facts = extractor.Extract(ctx, "testing");

  REQUIRE(facts.size() >= 2);

  bool has_file_path = false;
  bool has_error_msg = false;
  for (const auto& f : facts) {
    if (f.type == Artifact::Type::kFilePath) has_file_path = true;
    if (f.type == Artifact::Type::kErrorMsg) has_error_msg = true;
  }

  REQUIRE(has_file_path);
  REQUIRE(has_error_msg);
}
