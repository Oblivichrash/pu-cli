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

boost::json::value UsageLedger::Serialize() const {
  boost::json::object out;
  for (const auto& [node_id, usage] : entries_) {
    boost::json::object entry{
        {kRoundsKey, usage.rounds},
        {kPromptKey, usage.prompt_tokens},
        {kCompletionKey, usage.completion_tokens},
        {kCachedKey, usage.cached_tokens},
    };
    // Absent where the product reported no price: a turn that cost nothing knows it did, and
    // one whose product says nothing does not, and the two must not look alike.
    if (usage.cost) {
      entry["cost"] = boost::json::object{
          {kCostAmountKey, usage.cost->amount},
          {kCostUnitKey, usage.cost->unit},
      };
    }
    // Empty means "the provider did not say", which is not the same as a name.
    if (!usage.model.empty()) entry[kModelKey] = usage.model;
    if (!usage.gateway.empty()) entry[kGatewayKey] = usage.gateway;
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
