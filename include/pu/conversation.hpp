// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace pu {

struct ChatMessage {
  int id = 0;
  std::string timestamp;
  std::string role;
  std::string content;
  std::string tool_name;         // tool messages: name of the tool that produced the result
  std::string tool_calls_json;   // Serialized tool_calls for assistant messages
  std::string reasoning_content; // for DeepSeek thinking mode
  std::string tool_call_id;      // for tool messages: ID of the tool call

  NLOHMANN_DEFINE_TYPE_INTRUSIVE(ChatMessage, id, timestamp, role, content,
                                 tool_name, tool_calls_json, reasoning_content,
                                 tool_call_id)
};

}  // namespace pu
