// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/anthropic_provider.hpp"

#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/core/platform.hpp"

#include <spdlog/spdlog.h>

namespace pu {

namespace {

// The Anthropic request carries a block list rather than a string, so every message goes
// through a translation the OpenAI path does not need. `tool_result` is a block here rather
// than a role: a message the session stored as `role: "tool"` becomes a user-turn block
// naming the call it answers.
boost::json::value ToolResultBlock(const ChatMessage& message) {
  boost::json::value block = {{"type", "tool_result"}, {"tool_use_id", message.tool_call_id}};
  if (!message.Text().empty()) block.as_object()["content"] = message.Text();
  return block;
}

boost::json::value ToolUseBlock(const boost::json::value& call) {
  const std::string id = json::ValueOrDefault<std::string>(call, "id", "");
  const boost::json::value& function = call.at("function");
  const std::string name = json::ValueOrDefault<std::string>(function, "name", "");

  boost::json::value input = boost::json::object{};
  const boost::json::value& raw =
      json::ValueOrDefault<boost::json::value>(function, "arguments", boost::json::object{});
  if (raw.is_object()) {
    input = raw;
  } else if (raw.is_string()) {
    // Stored arguments are a string on the OpenAI wire and an object here. A blob that does
    // not parse is passed as the string it is rather than dropped: the tool refusing an
    // argument it cannot read is a better failure than one called with an empty object.
    const std::string text = boost::json::value_to<std::string>(raw);
    if (std::optional<boost::json::value> parsed = json::TryParse(text)) {
      input = std::move(*parsed);
    } else if (!text.empty()) {
      input = text;
    }
  }
  return boost::json::value{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", input}};
}

// One stored message as one Anthropic message. A `tool` role becomes a user turn holding a
// `tool_result` block, because this API has no separate role for one; a message whose only
// content is a tool call carries no `text` block, since an empty one is rejected.
// The bytes of an attachment, or a refusal. Same rule as the shared projection: a picture whose
// buffer is gone is reported, not sent as an empty base64 string.
std::string PayloadOf(const ContentBlock& block, const llm::PayloadLookup& lookup) {
  if (block.HasPayload()) return block.data;
  if (lookup) {
    std::string payload = lookup(block);
    if (!payload.empty()) return payload;
  }
  throw RequestRefused("the attachment '" + (block.name.empty() ? block.media_type : block.name) +
                       "' is no longer in the store, so this turn cannot send it. Run the "
                       "command that read it again, or start a new conversation");
}

boost::json::value ProjectMessage(const ChatMessage& message, const llm::PayloadLookup& lookup) {
  if (message.role == kToolRole) {
    boost::json::array blocks;
    blocks.push_back(ToolResultBlock(message));
    return boost::json::value{{"role", kUserRole}, {"content", std::move(blocks)}};
  }

  boost::json::array blocks;
  // A picture, or a document, before the words: this API states a block list in the order the
  // content appeared, and an attachment a turn opened with came before its question.
  for (const ContentBlock& block : message.content) {
    if (block.kind == BlockKind::kText) continue;
    if (block.kind == BlockKind::kImage) {
      blocks.push_back(boost::json::value{
          {"type", "image"},
          {"source", boost::json::object{{"type", "base64"},
                                         {"media_type", block.media_type},
                                         {"data", PayloadOf(block, lookup)}}},
      });
    } else {
      // This wire's own document block: the bytes, and the name a reader sees.
      blocks.push_back(boost::json::value{
          {"type", "document"},
          {"source", boost::json::object{{"type", "base64"},
                                         {"media_type", block.media_type},
                                         {"data", PayloadOf(block, lookup)}}},
          {"title", block.name},
      });
    }
  }
  // The thinking first, before the text and the calls, which is the order this API states a
  // block list in and the order the reply carried. It was dropped entirely: the OpenAI and
  // Ollama paths re-send theirs, this one built its own block list and had no branch for it, so
  // a turn that thought and then called a tool reached the next round without the reasoning
  // that produced the call. The signature travels with the text because this API verifies it.
  for (const ReasoningBlock& block : message.reasoning) {
    if (block.text.empty()) continue;
    blocks.push_back(boost::json::value{
        {"type", "thinking"}, {"thinking", block.text}, {"signature", block.signature}});
  }
  if (!message.Text().empty()) {
    blocks.push_back(boost::json::value{{"type", "text"}, {"text", message.Text()}});
  }
  if (message.HasToolCalls()) {
    for (const boost::json::value& call : message.tool_calls.as_array()) {
      if (!json::HasKey(call, "function") || !call.at("function").is_object()) continue;
      blocks.push_back(ToolUseBlock(call));
    }
  }

  // A message with nothing in it is still a message this API accepts, and dropping it would
  // reorder the turn. An empty text block is what it is given.
  if (blocks.empty()) blocks.push_back(boost::json::value{{"type", "text"}, {"text", ""}});
  return boost::json::value{{"role", message.role}, {"content", std::move(blocks)}};
}

// Thinking is a top-level field on the request rather than `extra_body`, and this API states
// a budget in tokens where the rest of this tree names a level. The mapping is deliberately
// lossy and stated: an absent level sends nothing, `none` omits the field entirely because
// this API has no way to say "do not think", and the three levels become budgets.
boost::json::value ThinkingField(ThinkingLevel level, int max_tokens) {
  if (level == ThinkingLevel::kServerDefault || level == ThinkingLevel::kNone) return nullptr;
  int budget = 1024;
  if (level == ThinkingLevel::kMedium) budget = 4096;
  if (level == ThinkingLevel::kHigh) budget = std::max(1024, max_tokens / 2);
  // A budget may not consume the whole cap, so the cap is raised to fit the budget rather
  // than the budget trimmed: a reply whose cap is smaller than its thinking is refused.
  return boost::json::value{{"type", "enabled"}, {"budget_tokens", budget}};
}

}  // namespace

AnthropicProvider::AnthropicProvider(Config config, std::shared_ptr<pu::http::HttpClient> http)
    : StreamingProvider(config.host, config.api_key, std::move(http)),
      config_(std::move(config)) {}

std::vector<std::string> AnthropicProvider::Headers() const {
  // Built rather than extended: the base class answers with a bearer, and this API reads the
  // key from `x-api-key` and refuses a request that carries both.
  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!config_.api_key.empty()) headers.push_back("x-api-key: " + config_.api_key);
  headers.push_back(std::string("anthropic-version: ") + kApiVersion);
  return headers;
}

std::string AnthropicProvider::BuildRequest(std::span<const ChatMessage> history,
                                            std::span<const ToolDefinition> tools) const {
  boost::json::value req = {
      {"model", config_.model},
      {"stream", true},
      {"max_tokens", config_.max_tokens},
      {"temperature", config_.temperature},
  };

  if (config_.top_p) req.as_object()["top_p"] = *config_.top_p;
  if (!config_.stop.empty()) req.as_object()["stop_sequences"] = boost::json::value_from(config_.stop);

  if (boost::json::value thinking = ThinkingField(config_.thinking, config_.max_tokens);
      !thinking.is_null()) {
    req.as_object()["thinking"] = std::move(thinking);
    // This API refuses a temperature beside thinking: it is the one place the two sampler
    // controls cannot both be sent, and the level the caller named outranks the default.
    req.as_object().erase("temperature");
  }

  if (!config_.system_prompt.empty()) req.as_object()["system"] = config_.system_prompt;

  boost::json::array messages;
  for (const ChatMessage& message : history) {
    // The system prompt is a field of its own on this API, and a message with that role is
    // refused, so one that reached here is folded into the field rather than dropped.
    if (message.role == kSystemRole) {
      continue;
    }
    messages.push_back(ProjectMessage(message, PayloadForTurn()));
  }
  req.as_object()["messages"] = std::move(messages);

  if (!tools.empty()) {
    boost::json::array tools_json;
    for (const auto& tool : tools) {
      // `input_schema` rather than `parameters`, and no `type: function` wrapper: the shape
      // differs rather than the name.
      tools_json.push_back(boost::json::value{
          {"name", tool.name}, {"description", tool.description}, {"input_schema", tool.Parameters()}});
    }
    req.as_object()["tools"] = std::move(tools_json);
  }

  return boost::json::serialize(req);
}

void AnthropicProvider::ResetTurnState() {
  open_ = OpenBlock::kNone;
  open_tool_ = OpenToolUse{};
  pending_signature_.clear();
}

void AnthropicProvider::ParseData(std::string_view event, std::string_view data,
                                  const ChatRequest& request) {
  if (data.empty()) return;
  const std::optional<boost::json::value> j = json::TryParse(data);
  if (!j) {
    // One frame lost is not the turn lost; the reply so far stands.
    spdlog::warn("Skipping invalid JSON line from Anthropic");
    return;
  }
  HandleEvent(event, *j, request);
}

void AnthropicProvider::HandleEvent(std::string_view event, const boost::json::value& j,
                                    const ChatRequest& request) {
  static constexpr std::string_view kReadKeys[] = {
      "type", "index", "content_block", "delta", "message", "usage", "error",
  };
  NoteUnreadFields(j, kReadKeys);

  // This API reports a failure as an event of its own rather than as a field on a frame that
  // otherwise reads normally, so it is checked before the name is dispatched on.
  if (event == "error" || (json::HasKey(j, "type") &&
                           json::ValueOrDefault<std::string>(j, "type", "") == "error")) {
    const std::string detail = json::HasKey(j, "error") ? json::ErrorMessage(j.at("error"))
                                                        : json::ErrorMessage(j);
    throw GenericError("provider error: " + (detail.empty() ? boost::json::serialize(j) : detail));
  }

  if (event == "content_block_start") {
    HandleContentBlockStart(j, request);
  } else if (event == "content_block_delta") {
    HandleContentBlockDelta(j, request);
  } else if (event == "content_block_stop") {
    CloseOpenBlock(request);
  } else if (event == "message_delta") {
    HandleMessageDelta(j);
  } else if (event == "message_start") {
    // The first event states the model and the input counts; the output counts arrive later.
    if (json::HasKey(j, "message") && j.at("message").is_object()) {
      const boost::json::value& message = j.at("message");
      acc_.response_model = json::ValueOrDefault<std::string>(message, "model", "");
      if (json::HasKey(message, "usage") && message.at("usage").is_object()) {
        TokenUsage counted = acc_.usage.value_or(TokenUsage{});
        counted.prompt_tokens = json::ValueOrDefault<int>(message.at("usage"), "input_tokens", 0);
        acc_.usage = counted;
        acc_.raw_usage = message.at("usage");
      }
    }
  } else if (event == "message_stop") {
    CloseOpenBlock(request);
  }
  // `ping` and an unknown event name are both nothing to do: the first is a keepalive and the
  // second is a name this build has not been taught, which is reported by `NoteUnreadFields`
  // through the payload it came with rather than guessed at.
}

void AnthropicProvider::HandleContentBlockStart(const boost::json::value& j,
                                                const ChatRequest& request) {
  // A `content_block_start` for a block that is still open is a stream that skipped its stop;
  // closing it first keeps the block list in the order the stream wrote it.
  CloseOpenBlock(request);

  if (!json::HasKey(j, "content_block") || !j.at("content_block").is_object()) return;
  const boost::json::value& block = j.at("content_block");
  const std::string type = json::ValueOrDefault<std::string>(block, "type", "");

  if (type == "tool_use") {
    open_ = OpenBlock::kToolUse;
    open_tool_ = OpenToolUse{};
    open_tool_.id = json::ValueOrDefault<std::string>(block, "id", "");
    open_tool_.name = json::ValueOrDefault<std::string>(block, "name", "");
    // The input arrives as deltas, so a `start` that states one is either an empty object (the
    // usual case, and not a prefix of what follows) or the whole input on a server that does
    // not stream it. Taking it as a starting value would put `{}` in front of every fragment.
    if (json::HasKey(block, "input") && block.at("input").is_object()) {
      const boost::json::object& input = block.at("input").as_object();
      if (!input.empty()) open_tool_.arguments = boost::json::serialize(input);
    }
  } else if (type == "thinking") {
    open_ = OpenBlock::kThinking;
    pending_signature_.clear();
  } else if (type == "text") {
    open_ = OpenBlock::kText;
    // A start carries the first fragment of the text on some servers and nothing on others.
    const std::string first = json::ValueOrDefault<std::string>(block, "text", "");
    if (!first.empty()) AppendContent(first, request);
  } else {
    open_ = OpenBlock::kOther;
  }
}

void AnthropicProvider::HandleContentBlockDelta(const boost::json::value& j,
                                                const ChatRequest& request) {
  if (!json::HasKey(j, "delta") || !j.at("delta").is_object()) return;
  const boost::json::value& delta = j.at("delta");
  const std::string type = json::ValueOrDefault<std::string>(delta, "type", "");

  if (type == "text_delta") {
    AppendContent(json::ValueOrDefault<std::string>(delta, "text", ""), request);
  } else if (type == "thinking_delta") {
    // The signature is not known until the block closes, so the text is accumulated without
    // one and the block is completed on `content_block_stop`. The accumulator merges adjacent
    // text into one block, which is what this API's blocks are.
    AppendReasoning(json::ValueOrDefault<std::string>(delta, "thinking", ""), request);
  } else if (type == "signature_delta") {
    pending_signature_ += json::ValueOrDefault<std::string>(delta, "signature", "");
  } else if (type == "input_json_delta") {
    // The tool's input arrives as string fragments that concatenate to one JSON object, the
    // same accumulation the OpenAI path does with `arguments`.
    open_tool_.arguments += json::ValueOrDefault<std::string>(delta, "partial_json", "");
  }
}

void AnthropicProvider::HandleMessageDelta(const boost::json::value& j) {
  if (json::HasKey(j, "delta") && j.at("delta").is_object()) {
    const boost::json::value& delta = j.at("delta");
    acc_.finish_reason =
        json::ValueOrDefault<std::string>(delta, "stop_reason", acc_.finish_reason);
  }
  if (json::HasKey(j, "usage") && j.at("usage").is_object()) {
    const boost::json::value& usage = j.at("usage");
    TokenUsage counted = acc_.usage.value_or(TokenUsage{});
    // The output count arrives here and replaces whatever the start event left at zero;
    // `input_tokens` is stated once on `message_start` and again here, and this is the later
    // word, so it is taken.
    if (json::HasKey(usage, "output_tokens")) {
      counted.completion_tokens = json::ValueOrDefault<int>(usage, "output_tokens", 0);
    }
    if (json::HasKey(usage, "input_tokens")) {
      counted.prompt_tokens = json::ValueOrDefault<int>(usage, "input_tokens", 0);
    }
    if (json::HasKey(usage, "cache_read_input_tokens")) {
      counted.cached_tokens = json::ValueOrDefault<int>(usage, "cache_read_input_tokens", 0);
    }
    // This API states the two sides rather than their sum, and this tree does not derive a
    // total a provider never gave.
    acc_.usage = counted;
    acc_.raw_usage = usage;
  }
}

void AnthropicProvider::CloseOpenBlock(const ChatRequest& request) {
  if (open_ == OpenBlock::kToolUse) {
    ToolCall call;
    call.id = open_tool_.id;
    call.name = open_tool_.name;
    if (!open_tool_.arguments.empty()) {
      if (std::optional<boost::json::value> parsed = json::TryParse(open_tool_.arguments)) {
        call.arguments = std::move(*parsed);
      } else {
        // Half-written arguments are a call that never finished; a truncated one is not one
        // to run a tool on. `max_tokens` is the cap that causes it here.
        if (acc_.finish_reason == "max_tokens") {
          throw GenericError("tool call '" + call.name + "' was cut off by the output token cap");
        }
        call.arguments = open_tool_.arguments;
      }
    }
    AppendToolCall(std::move(call), request);
    open_tool_ = OpenToolUse{};
  }
  open_ = OpenBlock::kNone;
}

void AnthropicProvider::FinishStream(const ChatRequest& request) {
  // A stream that ended without its `content_block_stop` still had an open block, and the
  // call it was building is one the next turn would otherwise never see.
  CloseOpenBlock(request);

  // The signature is attached now because a block closed before it arrived is a block whose
  // signature this build never saw, and inventing one would be replaying a verification the
  // provider never issued.
  if (!pending_signature_.empty() && !acc_.reasoning.empty()) {
    acc_.reasoning.back().signature = pending_signature_;
  }
  pending_signature_.clear();
}

}  // namespace pu
