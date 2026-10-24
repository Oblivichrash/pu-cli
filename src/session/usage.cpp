// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/usage.hpp"

#include <utility>

#include "pu/core/json.hpp"

namespace pu::session {

namespace {

constexpr const char* kNodeKey = "node";
constexpr const char* kRoundsKey = "rounds";
constexpr const char* kPromptKey = "prompt_tokens";
constexpr const char* kCompletionKey = "completion_tokens";
constexpr const char* kCachedKey = "cached_tokens";
constexpr const char* kCostAmountKey = "amount";
constexpr const char* kCostUnitKey = "unit";
constexpr const char* kModelKey = "model";
constexpr const char* kGatewayKey = "gateway";

}  // namespace

void UsageLedger::Record(const TurnUsage& usage) {
  if (usage.node_id.empty()) return;
  entries_[usage.node_id] = usage;
}

std::map<std::string, TurnUsage> UsageLedger::KeepOnly(
    const std::map<std::string, bool>& live_node_ids) const {
  std::map<std::string, TurnUsage> kept;
  for (const auto& [node_id, usage] : entries_) {
    // A rewound turn is gone from the chain, and a cost for a message nobody can read is a
    // number with nothing to compare it to.
    if (live_node_ids.count(node_id) > 0) kept.emplace(node_id, usage);
  }
  return kept;
}

void UsageLedger::Clear() { entries_.clear(); }

std::optional<TurnUsage> UsageLedger::For(const std::string& node_id) const {
  const auto it = entries_.find(node_id);
  if (it == entries_.end()) return std::nullopt;
  return it->second;
}

SessionUsageTotal UsageLedger::Total() const {
  SessionUsageTotal total;
  bool any_cost_seen = false;
  bool any_cost_missing = false;
  for (const auto& [node_id, usage] : entries_) {
    ++total.turns;
    total.rounds += usage.rounds;
    total.prompt_tokens += usage.prompt_tokens;
    total.completion_tokens += usage.completion_tokens;
    total.cached_tokens += usage.cached_tokens;
    if (!usage.cost) {
      // A turn whose product said nothing is not a free turn: the total stops being a total.
      any_cost_missing = true;
      continue;
    }
    ++total.priced_turns;
    if (!any_cost_seen) {
      total.cost = usage.cost;
      any_cost_seen = true;
      continue;
    }
    if (total.cost->unit == usage.cost->unit) {
      total.cost->amount += usage.cost->amount;
    } else {
      // Two units are not comparable; keeping either would be a number in the wrong currency.
      total.cost.reset();
      any_cost_missing = true;
    }
  }
  if (any_cost_missing) total.cost.reset();
  return total;
}

boost::json::value UsageLedger::ToJson(const TurnUsage& usage) {
  boost::json::object out{
      {kPromptKey, usage.prompt_tokens},
      {kCompletionKey, usage.completion_tokens},
      {kCachedKey, usage.cached_tokens},
      {kRoundsKey, usage.rounds},
      {kModelKey, usage.model},
      {kGatewayKey, usage.gateway},
  };
  // A price only where the product reported one. Absent and zero are different answers:
  // a call a product priced at nothing is not a product that prices nothing.
  if (usage.cost) {
    out["cost"] = boost::json::object{
        {kCostAmountKey, usage.cost->amount},
        {kCostUnitKey, usage.cost->unit},
    };
  }
  return out;
}

boost::json::value UsageLedger::ToJson(const SessionUsageTotal& total) {
  boost::json::object out{
      {"turns", total.turns},
      {"rounds", total.rounds},
      {kPromptKey, total.prompt_tokens},
      {kCompletionKey, total.completion_tokens},
      {kCachedKey, total.cached_tokens},
      {"priced_turns", total.priced_turns},
  };
  if (total.cost) {
    out["cost"] = boost::json::object{
        {kCostAmountKey, total.cost->amount},
        {kCostUnitKey, total.cost->unit},
    };
  }
  return out;
}

boost::json::value UsageLedger::Serialize() const {
  boost::json::object out;
  for (const auto& [node_id, usage] : entries_) {
    boost::json::object entry = ToJson(usage).as_object();
    // Empty means "the provider did not say", which is not the same as a name, so a stored
    // file leaves the key out; a reader of one entry is entitled to a blank field.
    if (usage.model.empty()) entry.erase(kModelKey);
    if (usage.gateway.empty()) entry.erase(kGatewayKey);
    // The node is the key; repeating it inside lets a reader take one entry on its own.
    entry[kNodeKey] = node_id;
    out[node_id] = std::move(entry);
  }
  return out;
}

UsageLedger UsageLedger::Deserialize(const boost::json::value& j) {
  UsageLedger ledger;
  if (!j.is_object()) return ledger;
  for (const auto& [node_id, entry] : j.as_object()) {
    if (!entry.is_object()) continue;
    TurnUsage usage;
    usage.node_id = std::string(node_id);
    usage.rounds = json::ValueOrDefault<int>(entry, kRoundsKey, 0);
    usage.prompt_tokens = json::ValueOrDefault<int>(entry, kPromptKey, 0);
    usage.completion_tokens = json::ValueOrDefault<int>(entry, kCompletionKey, 0);
    usage.cached_tokens = json::ValueOrDefault<int>(entry, kCachedKey, 0);
    if (json::HasKey(entry, "cost") && entry.at("cost").is_object()) {
      const boost::json::value& cost = entry.at("cost");
      usage.cost = Cost{json::ValueOrDefault<double>(cost, kCostAmountKey, 0.0),
                        json::ValueOrDefault<std::string>(cost, kCostUnitKey, "")};
    }
    usage.model = json::ValueOrDefault<std::string>(entry, kModelKey, "");
    usage.gateway = json::ValueOrDefault<std::string>(entry, kGatewayKey, "");
    ledger.entries_.emplace(usage.node_id, std::move(usage));
  }
  return ledger;
}

}  // namespace pu::session
