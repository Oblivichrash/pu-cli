// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/projection.hpp"

#include "pu/core/json.hpp"
#include "pu/context/message.hpp"

namespace pu::llm {

namespace {

using context::kAssistantRole;
using context::kSystemRole;
using context::kToolRole;
using context::kUserRole;

std::string ProjectRole(const std::string& role, RoleNaming naming) {
  if (naming == RoleNaming::kAliasToolResult) {
    return role == "tool_result" ? kToolRole : role;
  }
  if (role == kUserRole || role == kAssistantRole || role == kSystemRole ||
      role == kToolRole) {
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
  try {
    return boost::json::parse(boost::json::value_to<std::string>(arguments));
  } catch (const std::exception&) {
    // A provider may send arguments that are not parseable JSON; keep what
    // arrived rather than dropping the call.
    return arguments;
  }
}

boost::json::value ProjectToolCalls(const boost::json::value& tool_calls,
                                    const ProviderCapabilities& capabilities) {
  boost::json::array projected;
  for (const boost::json::value& call : tool_calls.as_array()) {
    // The stored shape nests the function. Anything else is left alone rather
    // than rebuilt into a shape the call did not have.
    if (!json::HasKey(call, "function") || !call.at("function").is_object()) {
      projected.push_back(call);
      continue;
    }
    const context::ToolCallRecord record = context::ToolCallFromJson(call);

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

boost::json::value ProjectMessage(const ChatMessage& message,
                                  const ProviderCapabilities& capabilities) {
  const std::string role = ProjectRole(message.role, capabilities.role_naming);
  const bool has_tool_calls = message.HasToolCalls();

  boost::json::value projected = {{"role", role}};

  if (has_tool_calls && !capabilities.allows_content_with_tool_calls) {
    projected.as_object()["content"] = boost::json::value();
  } else {
    projected.as_object()["content"] = message.content;
  }

  if (capabilities.echo_reasoning_content && role == kAssistantRole &&
      !message.reasoning_content.empty()) {
    projected.as_object()["reasoning_content"] = message.reasoning_content;
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
    projected.as_object()["tool_calls"] =
        ProjectToolCalls(message.tool_calls, capabilities);
  }

  return projected;
}

boost::json::array ProjectMessages(const std::vector<ChatMessage>& history,
                                   const ProviderCapabilities& capabilities) {
  boost::json::array messages;
  messages.reserve(history.size());
  for (const ChatMessage& message : history) {
    messages.push_back(ProjectMessage(message, capabilities));
  }
  return messages;
}

}  // namespace pu::llm
