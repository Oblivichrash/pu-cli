// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "pu/config/gateway.hpp"
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

// How a provider's API accepts reasoning on echoed assistant turns. Blocks are always the
// storage shape; this only decides what goes back on the wire.
enum class ReasoningCarriage {
  kText,    // one string, blocks joined (OpenAI-compatible `reasoning_content`)
  kBlocks,  // one object per block (`{text, signature}`), for APIs that signed them
};

struct ProviderCapabilities {
  RoleNaming role_naming = RoleNaming::kKnownRolesOnly;
  bool echoes_reasoning = false;
  const char* reasoning_field = "reasoning_content";
  ReasoningCarriage reasoning_carriage = ReasoningCarriage::kText;
  bool allows_content_with_tool_calls = true;
  ToolArgumentsEncoding tool_arguments = ToolArgumentsEncoding::kJsonObject;
  bool tool_calls_carry_type = false;
  bool sends_tool_name = false;
  bool omits_empty_tool_call_id = true;
};

std::string ProjectReasoningText(const std::vector<ReasoningBlock>& blocks);

boost::json::value ProjectReasoning(const std::vector<ReasoningBlock>& blocks);

// Ollama takes a top-level `think` as either a boolean or a level name, and only while
// a level is set: `kServerDefault` is not representable, so callers must not send it.
boost::json::value OllamaThinkValue(ThinkingLevel level);

boost::json::value ProjectMessage(const ChatMessage& message,
                                  const ProviderCapabilities& capabilities);

boost::json::array ProjectMessages(std::span<const ChatMessage> history,
                                   const ProviderCapabilities& capabilities);

// The keys one reply frame carries, filed under the wire path that reaches them, so a key
// is reported at the path a reader would look for it. Each wire format answers this for
// itself: the shapes are different enough that one walk over both would have to guess which
// one a frame is, and a guess is wrong exactly when a gateway misbehaves, which is when the
// report matters.
//
// `protocol` selects the format. A caller that knows the gateway passes its protocol;
// nothing is inferred from the frame.
void ForEachReplyKeyAtPath(const boost::json::value& j, config::WireProtocol protocol,
                           const std::function<void(std::string_view path, std::string_view key)>&
                               on_key);

}  // namespace pu::llm
