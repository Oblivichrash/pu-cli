// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <boost/json.hpp>

#include "pu/paging/paging.hpp"
#include "pu/session/graph.hpp"
#include "pu/llm/llm_provider.hpp"

namespace pu::session {

struct RequestInputs {
  std::string system_prompt;  // the agent's configured prompt
  std::string environment;    // generated context: OS, policy, guidelines
};

inline ChatMessage RenderMessage(const session::MessageNode& node, int position) {
  ChatMessage msg;
  msg.id = position;
  msg.timestamp = node.timestamp;

  if (const auto* user = std::get_if<session::UserPayload>(&node.payload)) {
    msg.role = session::kUserRole;
    msg.content = user->content;
    msg.from_peer = user->from_peer;
  } else if (const auto* assistant = std::get_if<session::AssistantPayload>(&node.payload)) {
    msg.role = session::kAssistantRole;
    msg.content = assistant->content;
    for (const session::Reasoning& block : assistant->reasoning) {
      msg.reasoning.push_back(ReasoningBlock{block.text, block.signature, block.raw});
    }
    if (!assistant->tool_calls.empty()) {
      boost::json::array calls;
      for (const session::ToolCallRecord& record : assistant->tool_calls) {
        calls.push_back(session::ToolCallToJson(record));
      }
      msg.tool_calls = std::move(calls);
    }
  } else if (const auto* system = std::get_if<session::SystemPayload>(&node.payload)) {
    msg.role = session::kSystemRole;
    msg.content = system->content;
  } else {
    const session::ToolPayload& receipt = std::get<session::ToolPayload>(node.payload);
    msg.role = session::kToolRole;
    msg.content = receipt.content;
    msg.tool_name = receipt.tool_name;
    msg.tool_call_id = receipt.tool_call_id;
  }

  return msg;
}

inline std::vector<ChatMessage> BuildRequestPath(const session::MessageGraph& graph,
                                                 const RequestInputs& inputs) {
  std::vector<ChatMessage> messages;

  std::string system_text = inputs.system_prompt;
  if (!inputs.environment.empty()) {
    if (!system_text.empty()) system_text += "\n\n";
    system_text += inputs.environment;
  }
  if (!system_text.empty()) {
    ChatMessage system;
    system.role = session::kSystemRole;
    system.content = std::move(system_text);
    messages.push_back(std::move(system));
  }

  int position = static_cast<int>(messages.size()) + 1;

  // A page is superseded when a *newer run* of the same question was answered after it
  // was read. Then its text is replaced by a receipt, because a page re-sent on every
  // turn is what turns a long exploration into a full context. Three things survive:
  // a later page of the same run, since pages of one log are complementary rather than
  // successive; the newest run, always; and a page read *after* a newer run, since paging
  // back is a deliberate act and eliding what the model just asked for would only make it
  // ask again.
  const auto chain = graph.Chain();
  std::unordered_map<const session::MessageNode*, paging::Origin> origins;
  for (const session::MessageNode* node : chain) {
    if (!std::holds_alternative<session::ToolPayload>(node->payload)) continue;
    auto origin = paging::OriginOf(std::get<session::ToolPayload>(node->payload).content);
    if (origin) origins.emplace(node, std::move(*origin));
  }

  // Walking backwards keeps, per source, the newest run that was read after this point.
  struct Newer {
    std::size_t rank = 0;
    std::string buffer;
  };
  std::unordered_map<std::string, Newer> newer_behind;
  std::unordered_map<const session::MessageNode*, std::string> superseded_by;
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

  for (const session::MessageNode* node : chain) {
    ChatMessage msg = RenderMessage(*node, position++);
    const auto replaced = superseded_by.find(node);
    if (replaced != superseded_by.end()) {
      msg.content = paging::SupersededReceipt(msg.content, replaced->second);
    }
    messages.push_back(std::move(msg));
  }
  return messages;
}

}  // namespace pu::session
