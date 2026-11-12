// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/projection.hpp"

#include "pu/core/json.hpp"
#include "pu/session/message.hpp"

namespace pu::llm {

namespace {

using session::kAssistantRole;
using session::kSystemRole;
using session::kToolRole;
using session::kUserRole;

std::string ProjectRole(const std::string& role, RoleNaming naming) {
  if (naming == RoleNaming::kAsStored) return role;
  if (role == kUserRole || role == kAssistantRole || role == kSystemRole || role == kToolRole) {
    return role;
  }
  return kUserRole;
}

boost::json::value ProjectArguments(const boost::json::value& arguments,
                                    ToolArgumentsEncoding encoding) {
  if (encoding == ToolArgumentsEncoding::kJsonString) {
    return arguments.is_object() || arguments.is_array()
               ? boost::json::value(boost::json::serialize(arguments))
               : arguments;
  }
  if (!arguments.is_string()) return arguments;
  // A stored argument blob that is not JSON is echoed as the string it is: the gateway that
  // wrote it is the one that will read it back.
  std::optional<boost::json::value> parsed =
      json::TryParse(boost::json::value_to<std::string>(arguments));
  return parsed ? std::move(*parsed) : arguments;
}

boost::json::value ProjectToolCalls(const boost::json::value& tool_calls,
                                    const ProviderCapabilities& capabilities) {
  boost::json::array projected;
  for (const boost::json::value& call : tool_calls.as_array()) {
    if (!json::HasKey(call, "function") || !call.at("function").is_object()) {
      projected.push_back(call);
      continue;
    }
    const session::ToolCallRecord record = session::ToolCallFromJson(call);

    boost::json::value entry = boost::json::object{};
    if (json::HasKey(call, "id")) entry.as_object()["id"] = call.at("id");
    if (capabilities.tool_calls_carry_type) entry.as_object()["type"] = "function";
    entry.as_object()["function"] = boost::json::value{
        {"name", record.name},
        {"arguments", ProjectArguments(record.arguments, capabilities.tool_arguments)},
    };
    projected.push_back(std::move(entry));
  }
  return projected;
}

}  // namespace

std::string ProjectReasoningText(const std::vector<ReasoningBlock>& blocks) {
  std::string text;
  for (const ReasoningBlock& block : blocks) {
    if (!text.empty() && !block.text.empty()) text += "\n";
    text += block.text;
  }
  return text;
}

boost::json::value ProjectReasoning(const std::vector<ReasoningBlock>& blocks) {
  boost::json::array projected;
  for (const ReasoningBlock& block : blocks) {
    if (!block.raw.is_null()) {
      projected.push_back(block.raw);
      continue;
    }
    boost::json::object out = {{"text", block.text}};
    if (!block.signature.empty()) out["signature"] = block.signature;
    projected.push_back(std::move(out));
  }
  return projected;
}

boost::json::value OllamaThinkValue(ThinkingLevel level) {
  if (level == ThinkingLevel::kNone) return false;
  return ThinkingLevelName(level);
}

boost::json::value ProjectMessage(const ChatMessage& message,
                                  const ProviderCapabilities& capabilities) {
  const std::string role = ProjectRole(message.role, capabilities.role_naming);
  const bool has_tool_calls = message.HasToolCalls();
  const bool has_reasoning = !message.reasoning.empty();

  boost::json::value projected = {{"role", role}};

  if (has_tool_calls && !capabilities.allows_content_with_tool_calls) {
    projected.as_object()["content"] = boost::json::value();
  } else {
    projected.as_object()["content"] = message.content;
  }

  if (has_reasoning && role == kAssistantRole && capabilities.echoes_reasoning) {
    boost::json::value& slot = projected.as_object()[capabilities.reasoning_field];
    if (capabilities.reasoning_carriage == ReasoningCarriage::kText) {
      slot = ProjectReasoningText(message.reasoning);
    } else {
      slot = ProjectReasoning(message.reasoning);
    }
  }

  if (role == kToolRole) {
    if (capabilities.sends_tool_name) {
      projected.as_object()["tool_name"] = message.tool_name;
    }
    if (!capabilities.omits_empty_tool_call_id || !message.tool_call_id.empty()) {
      projected.as_object()["tool_call_id"] = message.tool_call_id;
    }
  }

  if (has_tool_calls) {
    projected.as_object()["tool_calls"] = ProjectToolCalls(message.tool_calls, capabilities);
  }

  return projected;
}

boost::json::array ProjectMessages(std::span<const ChatMessage> history,
                                   const ProviderCapabilities& capabilities) {
  boost::json::array messages;
  messages.reserve(history.size());
  for (const ChatMessage& message : history) {
    messages.push_back(ProjectMessage(message, capabilities));
  }
  return messages;
}

namespace {

using KeySink = std::function<void(std::string_view path, std::string_view key)>;

void EmitKeys(const boost::json::object& object, std::string_view path, const KeySink& on_key) {
  for (const auto& member : object) on_key(path, member.key());
}

void EmitKeys(const boost::json::object& object, std::string_view path, std::string_view member,
              const KeySink& on_key) {
  EmitKeys(object, std::string(path) + "." + std::string(member), on_key);
}

// OpenAI's reply: the frame carries `choices[0]`, whose `delta` (streaming) or `message`
// (whole) holds the reply, and `usage` holds the counts. A nested usage member names a
// second path, so `cached_tokens` is filed under `usage.prompt_tokens_details`.
void WalkOpenAIReply(const boost::json::object& frame, const KeySink& on_key) {
  EmitKeys(frame, "top-level", on_key);

  const auto choices = frame.find("choices");
  if (choices != frame.end() && choices->value().is_array() && !choices->value().as_array().empty()) {
    if (const boost::json::object* choice = choices->value().as_array().at(0).if_object()) {
      EmitKeys(*choice, "choice", on_key);
      for (std::string_view container : {"delta", "message"}) {
        const auto piece = choice->find(container);
        if (piece != choice->end() && piece->value().is_object()) {
          EmitKeys(piece->value().as_object(), "delta", on_key);
          break;
        }
      }
    }
  }

  const auto usage = frame.find("usage");
  if (usage != frame.end() && usage->value().is_object()) {
    for (const auto& member : usage->value().as_object()) {
      on_key("usage", member.key());
      if (member.value().is_object()) {
        EmitKeys(member.value().as_object(), "usage", member.key(), on_key);
      }
    }
  }
}

// Ollama's reply: every key is on the frame itself, and the reply rides in `message`. There
// is no `choices` and no `usage`: counts sit beside `done` at the top, so they are filed at
// the frame's own path along with everything else.
void WalkOllamaReply(const boost::json::object& frame, const KeySink& on_key) {
  EmitKeys(frame, "top-level", on_key);

  const auto message = frame.find("message");
  if (message != frame.end() && message->value().is_object()) {
    EmitKeys(message->value().as_object(), "message", on_key);
  }
}

}  // namespace

void ForEachReplyKeyAtPath(const boost::json::value& j, model::WireProtocol protocol,
                           const std::function<void(std::string_view, std::string_view)>& on_key) {
  if (!on_key) return;
  const boost::json::object* frame = j.if_object();
  if (frame == nullptr) return;
  switch (protocol) {
    case model::WireProtocol::kOllama:
      WalkOllamaReply(*frame, on_key);
      return;
    case model::WireProtocol::kOpenAI:
      WalkOpenAIReply(*frame, on_key);
      return;
  }
}

}  // namespace pu::llm
