// SPDX-License-Identifier: GPL-3.0-only

//
// A turn's cost, read back the way a page reads it.
//
// The page draws what `/api/history` answers with, so the cost has to travel with the turn it
// belongs to — beside it, in a field of its own, never inside the content the model reads.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <string>

#include "pu/core/json.hpp"
#include "pu/session/session.hpp"
#include "pu/session/usage.hpp"
#include "tests/mocks/serve_harness.hpp"

TEST_CASE("A stored turn's cost is what the page reads back", "[serve][api][usage]") {
  pu::tests::ServeHarness harness;
  pu::Runtime& runtime = harness.RuntimeRef();
  auto session = runtime.DefaultSession();
  REQUIRE(session != nullptr);
  session->Messages().Append("user", "hello");
  session->Messages().Append("assistant", "hi");
  const std::string node = session->Messages().Chain().back()->id;

  pu::session::TurnUsage usage;
  usage.node_id = node;
  usage.rounds = 2;
  usage.prompt_tokens = 18232;
  usage.completion_tokens = 204;
  usage.cached_tokens = 15360;
  usage.cost = pu::Cost{0.17, "credits"};
  usage.model = "test-model";
  usage.gateway = "ollama";
  session->Usage().Record(usage);

  const boost::json::value history = boost::json::parse(harness.Client().Get("/api/history"));
  REQUIRE(history.is_array());

  // Find the reply the cost was filed against.
  bool found = false;
  for (const boost::json::value& item : history.as_array()) {
    // `/api/history` names a message by its position; the ledger is keyed by the node id
    // behind it, so the reply the cost was filed against is found by its content.
    if (pu::json::ValueOrDefault<std::string>(item, "content", "") != "hi") continue;
    found = true;
    REQUIRE(item.as_object().contains("usage"));
    const boost::json::value& cost = item.at("usage");
    REQUIRE(pu::json::ValueOrDefault<int>(cost, "prompt_tokens", 0) == 18232);
    REQUIRE(pu::json::ValueOrDefault<int>(cost, "completion_tokens", 0) == 204);
    // The number a prompt change is judged by.
    REQUIRE(pu::json::ValueOrDefault<int>(cost, "cached_tokens", 0) == 15360);
    REQUIRE(cost.as_object().contains("cost"));
    const boost::json::value& price = cost.at("cost");
    REQUIRE(pu::json::ValueOrDefault<double>(price, "amount", 0.0) == 0.17);
    REQUIRE(pu::json::ValueOrDefault<std::string>(price, "unit", "") == "credits");
    REQUIRE(pu::json::ValueOrDefault<int>(cost, "rounds", 0) == 2);
    REQUIRE(pu::json::ValueOrDefault<std::string>(cost, "model", "") == "test-model");
    // The content the model reads is untouched by the cost beside it.
    REQUIRE(pu::json::ValueOrDefault<std::string>(item, "content", "") == "hi");
  }
  REQUIRE(found);
}

TEST_CASE("A turn with no recorded cost carries no usage field", "[serve][api][usage]") {
  pu::tests::ServeHarness harness;
  pu::Runtime& runtime = harness.RuntimeRef();
  auto session = runtime.DefaultSession();
  REQUIRE(session != nullptr);
  session->Messages().Append("user", "hello");
  session->Messages().Append("assistant", "hi");

  const boost::json::value history = boost::json::parse(harness.Client().Get("/api/history"));
  REQUIRE(history.is_array());
  REQUIRE(!history.as_array().empty());
  // Absent, not zero: a page can tell "nothing was billed here" from "this cost nothing".
  REQUIRE_FALSE(history.as_array().back().as_object().contains("usage"));
}
