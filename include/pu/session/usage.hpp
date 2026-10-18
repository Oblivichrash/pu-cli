// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/json.hpp>

#include "pu/llm/llm_provider.hpp"

#include <map>
#include <optional>
#include <string>

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

  boost::json::value Serialize() const;
  // Absent is not an error: a session written before this existed has no ledger, and one
  // written by a build that dropped it is read the same way.
  static UsageLedger Deserialize(const boost::json::value& j);

 private:
  std::map<std::string, TurnUsage> entries_;
};

}  // namespace pu::session
