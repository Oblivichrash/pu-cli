// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>
#include <functional>

#include <boost/json.hpp>

#include "pu/http_client.hpp"  // pu::CancelToken
#include "pu/json.hpp"

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

  // Boost.JSON value_from/value_to equivalents of the former
  // NLOHMANN_DEFINE_TYPE_INTRUSIVE(ChatMessage, ...) macro.
  friend void tag_invoke(boost::json::value_from_tag,
                         boost::json::value& jv,
                         const ChatMessage& m) {
    jv = {
      {"id", m.id},
      {"timestamp", m.timestamp},
      {"role", m.role},
      {"content", m.content},
      {"tool_name", m.tool_name},
      {"tool_calls_json", m.tool_calls_json},
      {"reasoning_content", m.reasoning_content},
      {"tool_call_id", m.tool_call_id},
    };
  }

  friend ChatMessage tag_invoke(boost::json::value_to_tag<ChatMessage>,
                                const boost::json::value& jv) {
    ChatMessage m;
    const boost::json::object* o = jv.if_object();
    if (o) {
      m.id = json::ValueOrDefault<int>(jv, "id", 0);
      m.timestamp = json::ValueOrDefault<std::string>(jv, "timestamp", "");
      m.role = json::ValueOrDefault<std::string>(jv, "role", "");
      m.content = json::ValueOrDefault<std::string>(jv, "content", "");
      m.tool_name = json::ValueOrDefault<std::string>(jv, "tool_name", "");
      m.tool_calls_json = json::ValueOrDefault<std::string>(jv, "tool_calls_json", "");
      m.reasoning_content = json::ValueOrDefault<std::string>(jv, "reasoning_content", "");
      m.tool_call_id = json::ValueOrDefault<std::string>(jv, "tool_call_id", "");
    }
    return m;
  }
};

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string parameters_schema;
};

struct ToolCall {
  std::string id;
  std::string name;
  boost::json::value arguments;
};

struct ChatResult {
  std::string content;
  std::vector<ToolCall> tool_calls;
  int input_tokens = 0;
  int output_tokens = 0;
  std::string reasoning_content;
};

class LLMProvider {
public:
  virtual ~LLMProvider() = default;

  virtual ChatResult Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback = nullptr,
    std::function<void(const ToolCall&)> tool_callback = nullptr,
    CancelToken cancel_token = nullptr
  ) = 0;

  virtual bool SupportsTools() const = 0;
  virtual std::string GetModelName() const = 0;
  virtual bool IsThinkingMode() const { return false; }
};

} // namespace pu
