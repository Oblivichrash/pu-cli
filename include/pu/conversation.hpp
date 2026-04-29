// SPDX-License-Identifier: GPL-3.0-only
//
// Conversation storage structures.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace pu {

struct ChatMessage {
  int id = 0;
  std::string timestamp;
  std::string role;      // "user", "chat", "bash", "system"
  std::string content;
};

struct Conversation {
  std::string id;
  std::string created_at;
  std::string updated_at;
  std::vector<ChatMessage> messages;
  std::unordered_map<std::string, std::vector<ChatMessage>> expert_histories;
};

}  // namespace pu
