// SPDX-License-Identifier: GPL-3.0-only

//
// A turn's cost, read back the way a page reads it.
//
// The page draws what `/api/history` answers with, so the cost has to travel with the turn it
// belongs to — beside it, in a field of its own, never inside the content the model reads.
//

#include <catch2/catch_approx.hpp>
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

  const boost::json::value history = boost::json::parse(harness.Client().Get("/api/history")).at("messages");
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

// What the page draws under the context bar. The session endpoint sums the ledger, and says so
// with a price only where every turn that was priced agreed on the unit.
TEST_CASE("serve API /api/session reports what the conversation has spent",
          "[serve][api][usage]") {
  pu::tests::ServeHarness harness;
  auto client = harness.Client();

  auto session = harness.RuntimeRef().DefaultSession();
  REQUIRE(session != nullptr);
  pu::session::TurnUsage first;
  first.node_id = "turn-1";
  first.rounds = 1;
  first.prompt_tokens = 1000;
  first.completion_tokens = 50;
  first.cached_tokens = 400;
  first.cost = pu::Cost{0.05, "credits"};
  session->Usage().Record(first);

  pu::session::TurnUsage second = first;
  second.node_id = "turn-2";
  second.prompt_tokens = 2000;
  second.cost = pu::Cost{0.10, "credits"};
  session->Usage().Record(second);

  const auto j = boost::json::parse(client.Get("/api/session"));
  REQUIRE(j.at("success") == true);
  const auto& total = j.at("usage_total");
  CHECK(total.at("turns") == 2);
  CHECK(total.at("prompt_tokens") == 3000);
  CHECK(total.at("completion_tokens") == 100);
  // Both turns were recorded with the same cached count, so the sum is twice it.
  CHECK(total.at("cached_tokens") == 800);
  CHECK(total.at("priced_turns") == 2);
  REQUIRE(total.as_object().contains("cost"));
  CHECK(total.at("cost").at("unit") == "credits");
  CHECK(total.at("cost").at("amount").as_double() ==
        Catch::Approx(0.15).epsilon(0.000001));

  // One unpriced turn and the money stops being a total, while the counts still add.
  pu::session::TurnUsage third;
  third.node_id = "turn-3";
  third.prompt_tokens = 500;
  session->Usage().Record(third);

  const auto after = boost::json::parse(client.Get("/api/session"));
  const auto& after_total = after.at("usage_total");
  CHECK(after_total.at("turns") == 3);
  CHECK(after_total.at("prompt_tokens") == 3500);
  CHECK(after_total.at("priced_turns") == 2);
  CHECK_FALSE(after_total.as_object().contains("cost"));
}

TEST_CASE("A turn with no recorded cost carries no usage field", "[serve][api][usage]") {
  pu::tests::ServeHarness harness;
  pu::Runtime& runtime = harness.RuntimeRef();
  auto session = runtime.DefaultSession();
  REQUIRE(session != nullptr);
  session->Messages().Append("user", "hello");
  session->Messages().Append("assistant", "hi");

  const boost::json::value history = boost::json::parse(harness.Client().Get("/api/history")).at("messages");
  REQUIRE(history.is_array());
  REQUIRE(!history.as_array().empty());
  // Absent, not zero: a page can tell "nothing was billed here" from "this cost nothing".
  REQUIRE_FALSE(history.as_array().back().as_object().contains("usage"));
}
