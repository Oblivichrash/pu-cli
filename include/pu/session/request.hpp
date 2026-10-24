// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include <boost/json.hpp>

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
  for (const session::MessageNode* node : graph.Chain()) {
    messages.push_back(RenderMessage(*node, position++));
  }
  return messages;
}

}  // namespace pu::session
