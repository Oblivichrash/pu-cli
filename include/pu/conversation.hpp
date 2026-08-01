// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace pu {

struct ChatMessage {
  int id = 0;
  std::string timestamp;
  std::string role;
  std::string content;
  std::string tool_name;         // for tool messages: name of the tool, or tool_call_id?
  std::string tool_calls_json;   // Serialized tool_calls for assistant messages
  std::string reasoning_content; // for DeepSeek thinking mode
  std::string tool_call_id;      // for tool messages: ID of the tool call

  NLOHMANN_DEFINE_TYPE_INTRUSIVE(ChatMessage, id, timestamp, role, content,
                                 tool_name, tool_calls_json, reasoning_content,
                                 tool_call_id)
};

struct Conversation {
  std::string id;
  std::string created_at;
  std::string updated_at;
  std::vector<ChatMessage> messages;
  std::unordered_map<std::string, std::vector<ChatMessage>> expert_histories;
};

}  // namespace pu
