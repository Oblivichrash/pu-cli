// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/request.hpp"

namespace pu::session {

ChatMessage RenderMessage(const context::MessageNode& node, int position) {
  ChatMessage msg;
  msg.id = position;
  msg.timestamp = node.timestamp;

  if (const auto* user = std::get_if<context::UserPayload>(&node.payload)) {
    msg.role = "user";
    msg.content = context::FlattenText(user->content);
  } else if (const auto* assistant =
                 std::get_if<context::AssistantPayload>(&node.payload)) {
    msg.role = "assistant";
    msg.content = context::FlattenText(assistant->content);
    if (assistant->reasoning) msg.reasoning_content = assistant->reasoning->raw_json;
    if (!assistant->tool_calls.empty()) {
      boost::json::array calls;
      for (const context::ToolCallRecord& record : assistant->tool_calls) {
        calls.push_back(boost::json::value{
            {"id", record.id},
            {"type", "function"},
            {"function", {{"name", record.name}, {"arguments", record.arguments}}},
        });
      }
      msg.tool_calls = std::move(calls);
    }
  } else if (const auto* system =
                 std::get_if<context::SystemPayload>(&node.payload)) {
    msg.role = "system";
    msg.content = context::FlattenText(system->content);
  } else {
    const context::ToolPayload& receipt = std::get<context::ToolPayload>(node.payload);
    msg.role = "tool";
    msg.content = context::FlattenText(receipt.content);
    msg.tool_name = receipt.tool_name;
    msg.tool_call_id = receipt.tool_call_id;
  }

  return msg;
}

namespace {

// The boundary may cut off a tool call that has no receipt yet, which would
// leave the model unable to act on it. Pull the boundary back until every call
// near it either has its receipt or is itself kept.
std::size_t PairingSafeTailStart(const std::vector<const context::MessageNode*>& chain,
                                 std::size_t tail_start, std::size_t head) {
  if (chain.empty()) return tail_start;
  tail_start = std::min(tail_start, chain.size() - 1);
  for (std::size_t i = tail_start; i > head; --i) {
    const auto* assistant =
        std::get_if<context::AssistantPayload>(&chain[i]->payload);
    if (assistant == nullptr || assistant->tool_calls.empty()) continue;

    bool all_matched = true;
    for (const context::ToolCallRecord& record : assistant->tool_calls) {
      if (record.id.empty()) continue;
      bool matched = false;
      for (std::size_t j = i + 1; j < chain.size(); ++j) {
        const auto* receipt = std::get_if<context::ToolPayload>(&chain[j]->payload);
        if (receipt != nullptr && receipt->tool_call_id == record.id) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        all_matched = false;
        break;
      }
    }
    if (!all_matched) tail_start = i;
  }
  return tail_start;
}

}  // namespace

std::vector<ChatMessage> BuildRequestPath(const context::MessageGraph& graph,
                                          const context::MessageId& leaf,
                                          const RequestInputs& inputs,
                                          std::optional<KeepRecent> selection) {
  std::vector<ChatMessage> messages;

  std::string system_text = inputs.system_prompt;
  if (!inputs.environment.empty()) {
    if (!system_text.empty()) system_text += "\n\n";
    system_text += inputs.environment;
  }
  if (!system_text.empty()) {
    ChatMessage system;
    system.role = "system";
    system.content = std::move(system_text);
    messages.push_back(std::move(system));
  }

  const auto position = [&] { return static_cast<int>(messages.size()) + 1; };
  const std::vector<const context::MessageNode*> chain = graph.ChainFrom(leaf);

  const bool trims =
      selection && chain.size() > selection->head + selection->tail;
  if (!trims) {
    for (const context::MessageNode* node : chain) {
      messages.push_back(RenderMessage(*node, position()));
    }
    return messages;
  }

  const std::size_t head = selection->head;
  const std::size_t tail_start = PairingSafeTailStart(
      chain, chain.size() - selection->tail, head);

  for (std::size_t i = 0; i < head; ++i) {
    messages.push_back(RenderMessage(*chain[i], position()));
  }
  if (tail_start > head) {
    ChatMessage omitted;
    omitted.role = "system";
    omitted.content = "[Compressed: " + std::to_string(tail_start - head) +
                      " messages omitted]";
    messages.push_back(std::move(omitted));
  }
  for (std::size_t i = tail_start; i < chain.size(); ++i) {
    messages.push_back(RenderMessage(*chain[i], position()));
  }
  return messages;
}

}  // namespace pu::session
