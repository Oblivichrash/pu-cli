// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/request.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "pu/paging/paging.hpp"

namespace pu::session {

namespace {

// A page is superseded when a *newer run* of the same question was answered after it was
// read. Then its text is replaced by a receipt, because a page re-sent on every turn is what
// turns a long exploration into a full context. Three things survive: a later page of the
// same run, since pages of one log are complementary rather than successive; the newest run,
// always; and a page read *after* a newer run, since paging back is a deliberate act and
// eliding what the model just asked for would only make it ask again.
std::unordered_map<const MessageNode*, std::string> SupersededPages(
    const std::vector<const MessageNode*>& chain) {
  std::unordered_map<const MessageNode*, paging::Origin> origins;
  for (const MessageNode* node : chain) {
    const auto* receipt = std::get_if<ToolPayload>(&node->payload);
    if (receipt == nullptr || !receipt->origin) continue;
    origins.emplace(node, *receipt->origin);
  }

  // Walking backwards keeps, per source, the newest run that was read after this point.
  struct Newer {
    std::size_t rank = 0;
    std::string buffer;
  };
  std::unordered_map<std::string, Newer> newer_behind;
  std::unordered_map<const MessageNode*, std::string> superseded_by;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const auto origin = origins.find(*it);
    if (origin == origins.end()) continue;
    const paging::Origin& page = origin->second;
    const auto seen = newer_behind.find(page.source);
    if (seen != newer_behind.end() && seen->second.rank > page.rank) {
      superseded_by.emplace(*it, seen->second.buffer);
    }
    auto& newest = newer_behind[page.source];
    if (page.rank > newest.rank) newest = Newer{page.rank, page.buffer};
  }
  return superseded_by;
}

// How many superseded receipts keep their cursor. A cursor is a standing invitation to spend a
// turn reading a page, and that invitation is worth keeping while the run is still recent
// enough to be argued from -- a plan-and-verify cycle is about six results long. Past that the
// verdict is what stops a model repeating a failed approach, and the cursor is what makes it
// pay to reread a result it has already been told the shape of.
constexpr std::size_t kKeepNewestCursors = 6;

// A result that is already a tombstone costs nothing to leave alone, and re-tombstoning it
// would not make it smaller.
bool IsTombstone(std::string_view content) {
  return content.find("\"elided\":\"tombstone\"") != std::string_view::npos;
}

// A receipt that has been superseded once: its text is gone and the run that replaced it is
// named, but the cursor back into this run survives.
bool IsSupersededReceipt(std::string_view content) {
  return content.find("\"superseded_by\"") != std::string_view::npos;
}

// The second tier, entered by position in the chain of superseded receipts rather than by a
// size. T1 keeps a cursor so the model can page back; T2 drops it, because the offer to
// reread something six results old is a turn spent on history the conversation has already
// moved past. What survives is the same either way: the command, the verdict and how much
// there was -- the sentence about what happened, never a way to ask for more.
//
// This is a count, not a byte budget, and deliberately so. A byte count cannot be turned into
// tokens without the model's tokenizer, so any threshold expressed in bytes is a number nobody
// can check against the window it is meant to protect. Six results is a fact about the shape of
// a turn; 256 KiB is not.
//
// Nothing is removed: the receipt stays where it was, which is what keeps each `tool_calls[].id`
// beside the `tool_call_id` that answers it.
void TombstoneOldReceipts(std::vector<ChatMessage>& messages) {
  std::vector<std::size_t> superseded_at;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    if (messages[i].role != kToolRole) continue;
    const std::string_view content = messages[i].content;
    if (IsTombstone(content) || !IsSupersededReceipt(content)) continue;
    superseded_at.push_back(i);
  }
  if (superseded_at.size() <= kKeepNewestCursors) return;

  // The newest receipts keep their cursors by position in the list of superseded results, not
  // by index in the transcript: a reply between them does not make one of them newer.
  const std::size_t tombstone_before = superseded_at.size() - kKeepNewestCursors;
  for (std::size_t k = 0; k < tombstone_before; ++k) {
    ChatMessage& msg = messages[superseded_at[k]];
    const std::string smaller = paging::TombstonedReceipt(msg.content);
    // A body that does not parse gains nothing from the rewrite, and replacing one opaque
    // string with another only loses the field names a provider might still validate.
    if (smaller.size() >= msg.content.size()) continue;
    msg.content = smaller;
  }
}

// How many tool calls a request keeps whole. T2 leaves the scaffolding of every call in place:
// its `id`, its `name` and, above all, its `arguments`, none of which is ever elided. Twenty
// `write_file` calls of two hundred lines each are four thousand lines re-sent every turn
// forever, on the input side of a call whose *result* was already reduced to a tombstone on the
// output side. That is the last pool with no bound, and it is the one nobody budgeted for.
//
// T3 is the only tier that removes anything, and it removes *pairs*. A `tool` message whose
// `tool_call_id` names no call is rejected by every provider, so the smallest legal unit is the
// assistant message's call together with the result that answered it. Six is the same length as
// the cursor rule above and for the same reason: a plan-and-verify cycle is about six results,
// and past that the conversation has moved to a different question.
//
// It must stay well *above* `kKeepNewestCursors`, though. T2 goes first and only fires on a
// superseded receipt that is older than the newest six, so a T3 that kept six would delete the
// very receipts T2 was about to shrink and leave T2 with nothing to do. Four of headroom leaves
// two superseded receipts past the cursor floor in the common case, which is what keeps both
// tiers observable at once.
constexpr std::size_t kKeepNewestCalls = kKeepNewestCursors + 4;

// The calls one assistant message makes, as the ids they will be answered by. A message may
// carry several, and a parallel batch is retired as a unit or not at all -- dropping one arm of
// a batch and keeping another would leave a `tool_call_id` with no call.
std::vector<std::string> CallIds(const ChatMessage& msg) {
  std::vector<std::string> ids;
  if (msg.role != kAssistantRole) return ids;
  const boost::json::array* calls = msg.tool_calls.if_array();
  if (calls == nullptr) return ids;
  for (const boost::json::value& call : *calls) {
    const boost::json::object* fields = call.if_object();
    if (fields == nullptr) continue;
    const auto it = fields->find("id");
    if (it == fields->end() || !it->value().is_string()) continue;
    ids.push_back(boost::json::value_to<std::string>(it->value()));
  }
  return ids;
}

// The third tier: retire the oldest tool-call pairs, whole. Everything T0, T1 and T2 do leaves
// the call in place and shrinks only what came back from it; this is the one place a message
// leaves the request entirely, and it is why the pool it addresses is finally bounded.
void RetireOldToolCalls(std::vector<ChatMessage>& messages) {
  // Each assistant message that makes calls, in transcript order, with the ids it will be
  // answered by. A call whose result never arrived (the turn was stopped, or the pair was
  // already retired) still counts here: its arguments are the thing being removed.
  struct CallSite {
    std::size_t index = 0;
    std::vector<std::string> ids;
  };
  std::vector<CallSite> sites;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    std::vector<std::string> ids = CallIds(messages[i]);
    if (!ids.empty()) sites.push_back(CallSite{i, std::move(ids)});
  }
  if (sites.size() <= kKeepNewestCalls) return;

  // The newest sites keep everything, arguments and results alike; the older ones go, and the
  // results that answered them go with them.
  std::vector<bool> drop(messages.size(), false);
  for (std::size_t k = 0; k + kKeepNewestCalls < sites.size(); ++k) {
    const CallSite& site = sites[k];
    drop[site.index] = true;
    for (std::size_t j = site.index + 1; j < messages.size(); ++j) {
      if (messages[j].role != kToolRole) continue;
      const auto named = std::find(site.ids.begin(), site.ids.end(), messages[j].tool_call_id);
      if (named != site.ids.end()) drop[j] = true;
    }
  }

  // A message that keeps *some* of its calls but not all cannot be written: the call and its
  // result are one unit. Such a site is kept whole, which is why the loop above marks a site's
  // own message rather than individual entries of its array.
  std::vector<ChatMessage> kept;
  kept.reserve(messages.size());
  for (std::size_t i = 0; i < messages.size(); ++i) {
    if (!drop[i]) kept.push_back(std::move(messages[i]));
  }
  messages = std::move(kept);
}

}  // namespace

ChatMessage RenderMessage(const session::MessageNode& node, int position) {
  ChatMessage msg;
  msg.id = position;
  msg.timestamp = node.timestamp;

  if (const auto* user = std::get_if<UserPayload>(&node.payload)) {
    msg.role = kUserRole;
    msg.content = user->content;
  } else if (const auto* assistant = std::get_if<AssistantPayload>(&node.payload)) {
    msg.role = kAssistantRole;
    msg.content = assistant->content;
    for (const Reasoning& block : assistant->reasoning) {
      msg.reasoning.push_back(ReasoningBlock{block.text, block.signature, block.raw});
    }
    if (!assistant->tool_calls.empty()) {
      boost::json::array calls;
      for (const ToolCallRecord& record : assistant->tool_calls) {
        calls.push_back(ToolCallToJson(record));
      }
      msg.tool_calls = std::move(calls);
    }
  } else if (const auto* system = std::get_if<SystemPayload>(&node.payload)) {
    msg.role = kSystemRole;
    msg.content = system->content;
  } else {
    const ToolPayload& receipt = std::get<ToolPayload>(node.payload);
    msg.role = kToolRole;
    msg.content = receipt.content;
    msg.tool_name = receipt.tool_name;
    msg.tool_call_id = receipt.tool_call_id;
  }

  return msg;
}

std::vector<ChatMessage> BuildRequestPath(const session::MessageGraph& graph,
                                          const RequestInputs& inputs) {
  std::vector<ChatMessage> messages;

  std::string system_text = inputs.system_prompt;
  if (!inputs.environment.empty()) {
    if (!system_text.empty()) system_text += "\n\n";
    system_text += inputs.environment;
  }
  if (!system_text.empty()) {
    ChatMessage system;
    system.role = kSystemRole;
    system.content = std::move(system_text);
    messages.push_back(std::move(system));
  }

  int position = static_cast<int>(messages.size()) + 1;

  const std::vector<const MessageNode*> chain = graph.Chain();
  const auto superseded_by = SupersededPages(chain);
  messages.reserve(messages.size() + chain.size());

  for (const MessageNode* node : chain) {
    ChatMessage msg = RenderMessage(*node, position++);
    const auto replaced = superseded_by.find(node);
    if (replaced != superseded_by.end()) {
      msg.content = paging::SupersededReceipt(msg.content, replaced->second);
    }
    messages.push_back(std::move(msg));
  }

  TombstoneOldReceipts(messages);
  // Last, because it is the only tier that removes anything and every tier above it is
  // expressed in terms of a call that is still there.
  RetireOldToolCalls(messages);
  return messages;
}

}  // namespace pu::session
