// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/json.hpp>

#include "pu/llm/llm_provider.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pu::session {

// What one turn cost, kept beside the conversation rather than inside it.
//
// Usage is a fact about the run, not about the message: a model never reads it, `History()`
// never renders it, and a page draws it beside the reply rather than as part of it. Keeping it
// out of MessageNode is what lets the transcript keep its shape while the costs are kept.
//
// It is stored because it is the only record of what an optimization changed: a prompt edited
// so the cache stops hitting, or a model swapped for one that costs three times as much, both
// show up here and nowhere else.
struct TurnUsage {
  // The assistant node this turn produced. A turn that ran tools produces several; `rounds`
  // is how many, and the counts are the whole turn's, not this node's.
  std::string node_id;
  int rounds = 0;
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int cached_tokens = 0;
  // What the turn cost, absent where the product reports no price. `unit` is the product's
  // own — two products' units are not comparable, and nothing here converts them.
  std::optional<Cost> cost;
  std::string model;
  std::string gateway;
};

// What a whole session spent, summed from its turns.
//
// The counts add up; a price does not add across units. `cost` is therefore absent as soon as
// two turns report prices in different units, or as soon as one turn reports none — a total
// that mixed them would read as the session's price when it is only part of it. When every turn
// agrees, the total carries that unit.
struct SessionUsageTotal {
  int turns = 0;
  int rounds = 0;
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int cached_tokens = 0;
  std::optional<Cost> cost;
  // How many turns reported a price at all, so a page can say "priced 3 of 5 turns".
  int priced_turns = 0;
};

// The usage of every turn a session has run, by node id.
//
// A key that is no longer in the chain — a rewound turn, a conversation cleared and started
// again — is dropped on save, so the file describes the conversation it sits beside.
class UsageLedger {
 public:
  void Record(const TurnUsage& usage);
  // Every entry whose node is still in `live_node_ids`, in the order it was recorded.
  std::map<std::string, TurnUsage> KeepOnly(const std::map<std::string, bool>& live_node_ids) const;
  // Forgets everything. `/clear` is a new conversation, and its costs start from nothing.
  void Clear();

  const std::map<std::string, TurnUsage>& Entries() const { return entries_; }
  std::optional<TurnUsage> For(const std::string& node_id) const;

  // The sum, with the rule above about units.
  SessionUsageTotal Total() const;

  // How a turn's usage is written for a reader. `/api/history` puts it beside the message it
  // belongs to and the socket's `usage` frame carries the same object, so the page parses one
  // shape whether a turn just ended or is being replayed: both read the ledger, and neither
  // assembles the object itself.
  static boost::json::value ToJson(const TurnUsage& usage);
  // The sum, written for the same reader. Absent counts of zero are still written: a total is
  // read as a whole, and a missing key would have to be guessed at.
  static boost::json::value ToJson(const SessionUsageTotal& total);

  boost::json::value Serialize() const;
  // Absent is not an error: a session written before this existed has no ledger, and one
  // written by a build that dropped it is read the same way.
  static UsageLedger Deserialize(const boost::json::value& j);

 private:
  std::map<std::string, TurnUsage> entries_;
};

}  // namespace pu::session
