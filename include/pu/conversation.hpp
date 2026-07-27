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
  std::string tool_name;
  std::string tool_calls_json;  // Serialized tool_calls for assistant messages
};

struct Conversation {
  std::string id;
  std::string created_at;
  std::string updated_at;
  std::vector<ChatMessage> messages;
  std::unordered_map<std::string, std::vector<ChatMessage>> expert_histories;
};

}  // namespace pu