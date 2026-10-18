// SPDX-License-Identifier: GPL-3.0-only

//
// What a turn cost, kept beside the conversation and read back with it.
//
// The ledger is not part of a message: a model never sees it, and `History()` is the same list
// with it or without. These cases hold that boundary, and hold the older sessions that predate
// it working as they did.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/json.hpp>

#include <string>

#include "pu/core/json.hpp"
#include "pu/session/session.hpp"
#include "pu/session/usage.hpp"

namespace {

pu::session::TurnUsage UsageOf(const std::string& node_id, int rounds, int prompt, int completion,
                               int cached, double credit) {
  pu::session::TurnUsage usage;
  usage.node_id = node_id;
  usage.rounds = rounds;
  usage.prompt_tokens = prompt;
  usage.completion_tokens = completion;
  usage.cached_tokens = cached;
  if (credit > 0.0) usage.cost = pu::Cost{credit, "credits"};
  usage.model = "test-model";
  usage.gateway = "ollama";
  return usage;
}

}  // namespace

TEST_CASE("A recorded turn survives a save and a load", "[session][usage]") {
  pu::Session session;
  session.Messages().Append("user", "hello");
  session.Messages().Append("assistant", "hi");

  const std::string node = session.Messages().Chain().back()->id;
  session.Usage().Record(UsageOf(node, /*rounds=*/2, /*prompt=*/18232, /*completion=*/204,
                                 /*cached=*/15360, /*credit=*/0.17));

  auto restored = pu::Session::Deserialize(session.Serialize());
  REQUIRE(restored != nullptr);

  const std::optional<pu::session::TurnUsage> usage = restored->Usage().For(node);
  REQUIRE(usage.has_value());
  REQUIRE(usage->rounds == 2);
  REQUIRE(usage->prompt_tokens == 18232);
  REQUIRE(usage->completion_tokens == 204);
  // The number a prompt change is judged by: how much of the prompt was already cached.
  REQUIRE(usage->cached_tokens == 15360);
  REQUIRE(usage->cost.has_value());
  REQUIRE(usage->cost->amount == 0.17);
  REQUIRE(usage->cost->unit == "credits");
  REQUIRE(usage->model == "test-model");
  REQUIRE(usage->gateway == "ollama");
}

TEST_CASE("The conversation does not carry the usage with it", "[session][usage]") {
  pu::Session session;
  session.Messages().Append("user", "hello");
  session.Messages().Append("assistant", "hi");
  const std::size_t before = session.Messages().History().size();

  session.Usage().Record(UsageOf(session.Messages().Chain().back()->id, 1, 10, 5, 0, 0.01));

  // What the model reads and what a page draws from the chain are unchanged by a cost being
  // known: the ledger sits beside the conversation, not inside it.
  REQUIRE(session.Messages().History().size() == before);
  REQUIRE(session.Messages().History().back().content == "hi");
}

TEST_CASE("A session written before usage was kept still loads", "[session][usage]") {
  pu::Session session;
  session.Messages().Append("user", "hello");
  session.Messages().Append("assistant", "hi");

  // The key is absent in every file this build wrote until now, and absent is not an error.
  const boost::json::value saved = session.Serialize();
  REQUIRE_FALSE(saved.as_object().contains("usage"));

  auto restored = pu::Session::Deserialize(saved);
  REQUIRE(restored != nullptr);
  REQUIRE(restored->Usage().Entries().empty());
}

TEST_CASE("A rewound turn's cost is dropped, and the rest is kept", "[session][usage]") {
  pu::Session session;
  session.Messages().Append("user", "first");
  session.Messages().Append("assistant", "one");
  const std::string first = session.Messages().Chain().back()->id;
  session.Usage().Record(UsageOf(first, 1, 100, 10, 0, 0.05));

  session.Messages().Append("user", "second");
  session.Messages().Append("assistant", "two");
  const std::string second = session.Messages().Chain().back()->id;
  session.Usage().Record(UsageOf(second, 3, 500, 40, 200, 0.20));

  REQUIRE(session.Usage().Entries().size() == 2);

  // `RewindBefore(n)` keeps the chain up to turn n-1: turn 4 is "assistant two", so rewinding
  // before it drops that reply and the question it answered.
  REQUIRE(session.Messages().RewindBefore(4));

  auto restored = pu::Session::Deserialize(session.Serialize());
  REQUIRE(restored != nullptr);
  // The turn that was stepped over is out of the chain; a cost for a message nobody can read
  // is a number with nothing to compare it to.
  REQUIRE(restored->Usage().For(first).has_value());
  REQUIRE_FALSE(restored->Usage().For(second).has_value());
}

TEST_CASE("A ledger reads what it wrote, field by field", "[session][usage]") {
  pu::session::UsageLedger ledger;
  ledger.Record(UsageOf("node-a", 2, 10, 20, 30, 0.5));
  ledger.Record(UsageOf("node-b", 1, 1, 2, 3, 0.0));

  const boost::json::value saved = ledger.Serialize();
  REQUIRE(saved.is_object());
  REQUIRE(saved.as_object().size() == 2);

  const pu::session::UsageLedger read = pu::session::UsageLedger::Deserialize(saved);
  REQUIRE(read.Entries().size() == 2);
  REQUIRE(read.For("node-a")->prompt_tokens == 10);
  REQUIRE(read.For("node-b")->cached_tokens == 3);

  // An entry with no node id is not a record: nothing could be filed against it.
  ledger.Record(pu::session::TurnUsage{});
  REQUIRE(ledger.Entries().size() == 2);
}
