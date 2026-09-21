// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// How a conversation becomes the message objects a provider receives. The
// differences between providers are data here rather than branches in each
// provider's request builder.

#include <vector>

#include <boost/json.hpp>

#include "pu/llm/llm_provider.hpp"

namespace pu::llm {

// How role names reach the wire.
enum class RoleNaming {
  // `tool_result` is sent as `tool`; every other role passes through unchanged.
  kAliasToolResult,
  // Only the four known roles are sent; anything else becomes `user`.
  kKnownRolesOnly,
};

// How tool call arguments reach the wire.
enum class ToolArgumentsEncoding {
  kJsonObject,
  kJsonString,
};

// Every field is read by the projection below, so a field without a consumer is
// a field that does not belong here yet.
struct ProviderCapabilities {
  RoleNaming role_naming = RoleNaming::kKnownRolesOnly;
  bool echo_reasoning_content = false;
  bool allows_content_with_tool_calls = true;
  ToolArgumentsEncoding tool_arguments = ToolArgumentsEncoding::kJsonObject;
  bool tool_calls_carry_type = false;
  bool sends_tool_name = false;
  bool omits_empty_tool_call_id = true;
};

// Renders one message as the object a provider receives.
boost::json::value ProjectMessage(const ChatMessage& message,
                                  const ProviderCapabilities& capabilities);

// Renders a conversation in order.
boost::json::array ProjectMessages(const std::vector<ChatMessage>& history,
                                   const ProviderCapabilities& capabilities);

}  // namespace pu::llm
