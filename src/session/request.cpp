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

std::vector<ChatMessage> BuildRequestPath(const context::MessageGraph& graph,
                                          const context::MessageId& leaf,
                                          const RequestInputs& inputs) {
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

  int position = static_cast<int>(messages.size()) + 1;
  for (const context::MessageNode* node : graph.ChainFrom(leaf)) {
    messages.push_back(RenderMessage(*node, position++));
  }
  return messages;
}

}  // namespace pu::session
