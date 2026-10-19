// SPDX-License-Identifier: GPL-3.0-only
#include <catch2/catch_test_macros.hpp>

#include "pu/session/usage.hpp"

#include <boost/json.hpp>

using pu::session::TurnUsage;
using pu::session::UsageLedger;
using pu::Cost;

namespace {

TurnUsage Turn(const std::string& node, int prompt, int completion, int cached,
               std::optional<Cost> cost) {
  TurnUsage usage;
  usage.node_id = node;
  usage.rounds = 1;
  usage.prompt_tokens = prompt;
  usage.completion_tokens = completion;
  usage.cached_tokens = cached;
  usage.cost = cost;
  return usage;
}

}  // namespace

TEST_CASE("A session total sums the counts of every turn", "[usage]") {
  UsageLedger ledger;
  ledger.Record(Turn("a", 100, 10, 40, Cost{0.05, "credits"}));
  ledger.Record(Turn("b", 250, 25, 0, Cost{0.15, "credits"}));

  const auto total = ledger.Total();
  CHECK(total.turns == 2);
  CHECK(total.rounds == 2);
  CHECK(total.prompt_tokens == 350);
  CHECK(total.completion_tokens == 35);
  CHECK(total.cached_tokens == 40);
  CHECK(total.priced_turns == 2);
  REQUIRE(total.cost.has_value());
  CHECK(total.cost->amount == 0.20);
  CHECK(total.cost->unit == "credits");
}

// A price is a sum only while the turns agree on the unit. Two products' units are not
// comparable, and keeping one of them would be a number in the wrong currency.
TEST_CASE("A total stops carrying a price when the units disagree", "[usage]") {
  UsageLedger ledger;
  ledger.Record(Turn("a", 10, 1, 0, Cost{0.05, "credits"}));
  ledger.Record(Turn("b", 10, 1, 0, Cost{0.02, "usd"}));

  const auto total = ledger.Total();
  CHECK(total.priced_turns == 2);
  CHECK_FALSE(total.cost.has_value());
  // The counts still add: only the money stops being a total.
  CHECK(total.prompt_tokens == 20);
}

// A turn whose product said nothing about price is not a free turn. A total that ignored it
// would read as the session's price when it is only part of it.
TEST_CASE("A total stops carrying a price when one turn reported none", "[usage]") {
  UsageLedger ledger;
  ledger.Record(Turn("a", 10, 1, 0, Cost{0.05, "credits"}));
  ledger.Record(Turn("b", 10, 1, 0, std::nullopt));

  const auto total = ledger.Total();
  CHECK(total.turns == 2);
  CHECK(total.priced_turns == 1);
  CHECK_FALSE(total.cost.has_value());
}

TEST_CASE("A session with no turns totals to nothing rather than to a price", "[usage]") {
  UsageLedger ledger;
  const auto total = ledger.Total();
  CHECK(total.turns == 0);
  CHECK(total.prompt_tokens == 0);
  CHECK_FALSE(total.cost.has_value());
}
