// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/core/context.hpp"
#include "pu/core/fact.hpp"
#include <nlohmann/json.hpp>

using namespace pu::core;

TEST_CASE("Context basic operations", "[context]") {
  Context ctx("test");
  ctx.Append("user", "Hello");
  ctx.Append("assistant", "Hi there!");

  REQUIRE(ctx.HistorySize() == 2);
  auto recent = ctx.Recent(1);
  REQUIRE(recent.size() == 1);
  REQUIRE(recent[0].role == "assistant");

  ctx.SetVar("foo", nlohmann::json("bar"));
  auto val = ctx.GetVar("foo");
  REQUIRE(val.has_value());
  REQUIRE(val->get<std::string>() == "bar");
}

TEST_CASE("Fact operations", "[context]") {
  Context ctx;
  Fact f(Fact::Type::kFilePath, "/tmp/data.csv", "user_input");
  ctx.AddFact(f);
  REQUIRE(ctx.GetFacts().size() == 1);

  auto paths = ctx.GetFactsByType(Fact::Type::kFilePath);
  REQUIRE(paths.size() == 1);
}
