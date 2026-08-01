// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>
#include "pu/session/workspace.hpp"
#include "pu/session/artifact.hpp"
#include <nlohmann/json.hpp>

using namespace pu;

TEST_CASE("Workspace basic operations", "[workspace]") {
  Workspace ctx("test");
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

TEST_CASE("Artifact operations", "[workspace]") {
  Workspace ctx;
  Artifact f;
  f.type = Artifact::Type::kFilePath;
  f.content = "/tmp/data.csv";
  f.source = "user_input";
  ctx.AddArtifact(f);
  REQUIRE(ctx.GetArtifacts().size() == 1);
}
