// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <vector>

#include <boost/json.hpp>

#include "pu/llm/llm_provider.hpp"

namespace pu::llm {

enum class RoleNaming {
  kAsStored,
  kKnownRolesOnly,
};

enum class ToolArgumentsEncoding {
  kJsonObject,
  kJsonString,
};

struct ProviderCapabilities {
  RoleNaming role_naming = RoleNaming::kKnownRolesOnly;
  bool echo_reasoning_content = false;
  bool allows_content_with_tool_calls = true;
  ToolArgumentsEncoding tool_arguments = ToolArgumentsEncoding::kJsonObject;
  bool tool_calls_carry_type = false;
  bool sends_tool_name = false;
  bool omits_empty_tool_call_id = true;
};

boost::json::value ProjectMessage(const ChatMessage& message,
                                  const ProviderCapabilities& capabilities);

boost::json::array ProjectMessages(const std::vector<ChatMessage>& history,
                                   const ProviderCapabilities& capabilities);

}  // namespace pu::llm
