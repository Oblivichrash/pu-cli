// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "pu/model/gateway.hpp"
#include "pu/llm/llm_types.hpp"

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

// How a gateway spells an attachment on the wire. Three products, three answers, and the one
// that has none is the reason this is an enumeration rather than a flag per kind: a projection
// that cannot carry a block has to say so, not drop it.
enum class AttachmentWire {
  // A content-block list: `{"type":"image_url","image_url":{"url":"data:..."}}` for a picture,
  // and a text block for anything else, since this wire has no document block.
  kOpenAIContentBlocks,
  // Anthropic's block list: `image` blocks with a base64 source, `document` blocks for a file.
  kAnthropicBlocks,
  // Ollama: the words stay a string and pictures ride beside them in `images`. A file has
  // nowhere to go, so a conversation that carries one is refused rather than trimmed.
  kOllamaImages,
};

// Where an attachment's bytes are. A message names one rather than carrying it, so the
// projection is given the one lookup it needs; empty when the buffer is gone, which is refused
// rather than sent as an empty picture.
using PayloadLookup = std::function<std::string(const ContentBlock&)>;

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
  AttachmentWire attachments = AttachmentWire::kOpenAIContentBlocks;
};

std::string ProjectReasoningText(const std::vector<ReasoningBlock>& blocks);

boost::json::value ProjectReasoning(const std::vector<ReasoningBlock>& blocks);

// Ollama takes a top-level `think` as either a boolean or a level name, and only while
// a level is set: `kServerDefault` is not representable, so callers must not send it.
boost::json::value OllamaThinkValue(ThinkingLevel level);

// The request a gateway receives for one message. A block this wire cannot carry is refused
// with `RequestRefused` rather than dropped: a conversation that silently lost its picture is
// a turn answered from a question nobody asked.
boost::json::value ProjectMessage(const ChatMessage& message,
                                  const ProviderCapabilities& capabilities,
                                  const PayloadLookup& lookup = {});

boost::json::array ProjectMessages(std::span<const ChatMessage> history,
                                   const ProviderCapabilities& capabilities,
                                   const PayloadLookup& lookup = {});

// The keys one reply frame carries, filed under the wire path that reaches them, so a key
// is reported at the path a reader would look for it. Each wire format answers this for
// itself: the shapes are different enough that one walk over both would have to guess which
// one a frame is, and a guess is wrong exactly when a gateway misbehaves, which is when the
// report matters.
//
// `protocol` selects the format. A caller that knows the gateway passes its protocol;
// nothing is inferred from the frame.
void ForEachReplyKeyAtPath(const boost::json::value& j, model::WireProtocol protocol,
                           const std::function<void(std::string_view path, std::string_view key)>&
                               on_key);

}  // namespace pu::llm
