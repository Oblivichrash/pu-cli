// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/openai_protocol.hpp"

#include "pu/llm/projection.hpp"
#include "pu/llm/streaming_json_parser.hpp"
#include "pu/core/platform.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

namespace pu {

namespace {

std::string StreamErrorDetail(const boost::json::value& error) {
  if (error.is_string()) return boost::json::value_to<std::string>(error);
  const std::string message = json::ErrorMessage(error);
  return message.empty() ? boost::json::serialize(error) : message;
}

// A payload carried over several `data:` lines is the gateway splitting one object, and
// the line that closes it is the one that parses.
bool ParsesAsJson(std::string_view text) {
  try {
    boost::json::parse(text);
    return true;
  } catch (const boost::system::system_error&) {
    return false;
  }
}

constexpr llm::ProviderCapabilities kCapabilities{
    .role_naming = llm::RoleNaming::kAsStored,
    .echoes_reasoning = true,
    .reasoning_field = "reasoning_content",
    .allows_content_with_tool_calls = false,
    .tool_arguments = llm::ToolArgumentsEncoding::kJsonString,
    .tool_calls_carry_type = true,
    .sends_tool_name = false,
    .omits_empty_tool_call_id = false,
};

}  // namespace

namespace llm {

std::vector<std::string> AbsentShapeFields(
    const ResponseShape& shape, const std::function<bool(std::string_view)>& seen,
    bool required_only) {
  std::vector<std::string> absent;
  const auto expect = [&](std::string_view field) {
    if (!field.empty() && !seen(field)) absent.emplace_back(std::string(field));
  };

  if (!required_only) expect(shape.content_field);
  if (!required_only) expect(shape.reasoning_field);
  if (!required_only) expect(shape.refusal_field);
  expect(shape.usage_field);
  expect(shape.prompt_tokens_field);
  expect(shape.completion_tokens_field);
  expect(shape.total_tokens_field);

  // One gateway nests the cached count, another puts it at the top level; either satisfies.
  if (!shape.cached_tokens_nested_field.empty() &&
      !seen(shape.cached_tokens_nested_field) && !seen(shape.cached_tokens_top_level_field)) {
    absent.emplace_back(std::string(shape.cached_tokens_nested_field) + " (nor " +
                        std::string(shape.cached_tokens_top_level_field) + ")");
  }
  expect(shape.finish_reason_field);
  return absent;
}

}  // namespace llm

OpenAIProtocol::OpenAIProtocol(const Config& config, std::shared_ptr<pu::http::HttpClient> http)
    : StreamingProvider(config.host, config.api_key, std::move(http)), config_(config) {}

void OpenAIProtocol::ResetAccumulators() {
  missing_fields_.clear();
  pending_tools_.clear();
  content_.clear();
  reasoning_.clear();
  refusal_.clear();
  finish_reason_.clear();
  response_model_.clear();
  tool_calls_.clear();
  usage_.reset();
  raw_usage_ = nullptr;
  unread_fields_.clear();
  inspected_frame_ = false;
  pending_data_.clear();
  reply_choice_index_ = -1;
  reported_extra_choice_ = false;
}

std::string OpenAIProtocol::BuildRequest(std::span<const ChatMessage> history,
                                         std::span<const ToolDefinition> tools) const {
  boost::json::value req = {
      {"model", config_.model},
      {"stream", true},
      {"stream_options", {{"include_usage", true}}},
      {"temperature", config_.temperature},
  };
  if (config_.max_tokens) req.as_object()["max_tokens"] = *config_.max_tokens;

  if (config_.thinking == ThinkingLevel::kNone) {
    boost::json::value extra_body = {{"thinking", {{"type", "disabled"}}}};
    req.as_object()["extra_body"] = extra_body;
  } else if (config_.thinking != ThinkingLevel::kServerDefault) {
    req.as_object()["reasoning_effort"] = ThinkingLevelName(config_.thinking);
  }

  req.as_object()["messages"] = llm::ProjectMessages(history, kCapabilities);

  if (!tools.empty()) {
    boost::json::array tools_json;
    for (const auto& tool : tools) {
      boost::json::value function_obj = {{"name", tool.name},
                                         {"description", tool.description},
                                         {"parameters", tool.Parameters()}};

      tools_json.push_back(boost::json::value{{"type", "function"}, {"function", function_obj}});
    }
    req.as_object()["tools"] = tools_json;
  }
  return boost::json::serialize(req);
}

void OpenAIProtocol::HandleJsonToken(const boost::json::value& j, const ChatRequest& request) {
  // The top-level names this reads, kept beside the reads themselves so a name added to
  // the wire has to pass them to go unnoticed. `done` is this protocol's own sentinel.
  static constexpr std::string_view kReadKeys[] = {
      "error", "choices", "usage", "model", "done",
  };
  NoteUnreadFields(j, kReadKeys);

  const boost::json::object* frame = j.if_object();
  if (frame != nullptr) {
    for (const auto& member : *frame) NoteSeenField(member.key());
    if (const auto it = frame->find("choices");
        it != frame->end() && it->value().is_array() && !it->value().as_array().empty()) {
      const boost::json::value& first = it->value().as_array().at(0);
      if (const boost::json::object* choice = first.if_object()) {
        for (const auto& member : *choice) NoteSeenField(member.key());
        if (const auto delta = choice->find("delta");
            delta != choice->end() && delta->value().is_object()) {
          for (const auto& member : delta->value().as_object()) NoteSeenField(member.key());
        }
      }
    }
    if (const auto usage = frame->find("usage");
        usage != frame->end() && usage->value().is_object()) {
      for (const auto& member : usage->value().as_object()) {
        NoteSeenField(member.key());
        if (member.value().is_object()) {
          for (const auto& nested : member.value().as_object()) NoteSeenField(nested.key());
        }
      }
    }
  }

  if (json::HasKey(j, "error")) {
    throw GenericError("provider error: " + StreamErrorDetail(j.at("error")));
  }

  bool is_final = false;
  if (json::HasKey(j, "done") && boost::json::value_to<bool>(j.at("done"))) is_final = true;

  if (json::HasKey(j, "choices") && j.at("choices").is_array()) {
    for (const boost::json::value& choice : j.at("choices").as_array()) {
      if (!choice.is_object()) continue;

      const int index = json::ValueOrDefault<int>(choice, "index", 0);
      if (reply_choice_index_ < 0) reply_choice_index_ = index;
      if (index != reply_choice_index_) {
        if (!reported_extra_choice_) {
          reported_extra_choice_ = true;
          spdlog::debug("Ignoring completion index {}: the request asked for one", index);
        }
        continue;
      }

      const boost::json::value* piece = nullptr;
      if (json::HasKey(choice, "delta")) {
        piece = &choice.at("delta");
      } else if (json::HasKey(choice, "message")) {
        piece = &choice.at("message");
      }

      if (piece != nullptr && piece->is_object()) {
        AppendContent(
            json::ValueOrDefault<std::string>(*piece, config_.shape.content_field, ""), request);

        const std::string refusal =
            json::ValueOrDefault<std::string>(*piece, config_.shape.refusal_field, "");
        if (!refusal.empty()) refusal_ += refusal;

        const std::string_view reasoning_field = config_.shape.reasoning_field;
        if (!reasoning_field.empty() && json::HasKey(*piece, reasoning_field) &&
            piece->at(reasoning_field).is_string()) {
          AppendReasoning(boost::json::value_to<std::string>(piece->at(reasoning_field)), request);
        }

        if (json::HasKey(*piece, "tool_calls") && piece->at("tool_calls").is_array()) {
          for (const auto& tc : piece->at("tool_calls").as_array()) {
            if (!tc.is_object()) continue;
            const std::string id = json::ValueOrDefault<std::string>(tc, "id", "");
            if (!id.empty() || pending_tools_.empty()) pending_tools_.emplace_back();
            ToolCallAccumulator& acc = pending_tools_.back();
            if (!id.empty()) acc.id = id;
            if (json::HasKey(tc, "function") && tc.at("function").is_object()) {
              auto name = json::ValueOrDefault<std::string>(tc.at("function"), "name", "");
              if (!name.empty()) acc.name = name;
              acc.arguments +=
                  json::ValueOrDefault<std::string>(tc.at("function"), "arguments", "");
            }
          }
        }
      }

      const std::string_view finish_field = config_.shape.finish_reason_field;
      if (!finish_field.empty() && json::HasKey(choice, finish_field) &&
          choice.at(finish_field).is_string()) {
        finish_reason_ = boost::json::value_to<std::string>(choice.at(finish_field));
      }
    }
  }

  const llm::ResponseShape& shape = config_.shape;
  if (!shape.usage_field.empty() && json::HasKey(j, shape.usage_field) &&
      j.at(shape.usage_field).is_object()) {
    const boost::json::value& usage = j.at(shape.usage_field);
    TokenUsage counted;
    counted.prompt_tokens = json::ValueOrDefault<int>(usage, shape.prompt_tokens_field, 0);
    counted.completion_tokens = json::ValueOrDefault<int>(usage, shape.completion_tokens_field, 0);
    if (!shape.total_tokens_field.empty()) {
      counted.total_tokens = json::ValueOrDefault<int>(usage, shape.total_tokens_field, 0);
    }
    // The cached prompt is named in the nested OpenAI field by one gateway and at the
    // top level by another; both carry the same count.
    if (!shape.cached_tokens_parent.empty() && json::HasKey(usage, shape.cached_tokens_parent)) {
      counted.cached_tokens = json::ValueOrDefault<int>(usage.at(shape.cached_tokens_parent),
                                                        shape.cached_tokens_nested_field, 0);
    }
    if (counted.cached_tokens == 0 && !shape.cached_tokens_top_level_field.empty()) {
      counted.cached_tokens =
          json::ValueOrDefault<int>(usage, shape.cached_tokens_top_level_field, 0);
    }
    counted.credit = json::ValueOrDefault<double>(usage, shape.credit_field, 0.0);
    usage_ = counted;
    raw_usage_ = usage;
  }

  if (json::HasKey(j, "model") && j.at("model").is_string()) {
    response_model_ = boost::json::value_to<std::string>(j.at("model"));
  }

  if (is_final) FlushPendingToolCalls(request);
}

void OpenAIProtocol::FlushPendingToolCalls(const ChatRequest& request) {
  for (ToolCallAccumulator& acc : pending_tools_) {
    ToolCall call;
    call.id = acc.id;
    call.name = acc.name;
    if (!acc.arguments.empty()) {
      try {
        call.arguments = boost::json::parse(acc.arguments);
      } catch (const boost::system::system_error&) {
        // The cap that ended the reply is what left the arguments half written, so
        // the call never finished; keeping the fragment would run a tool on a
        // truncated argument. A backend that answers with something other than JSON
        // is a different thing, and its text is kept as it stands.
        if (finish_reason_ == "length") {
          throw GenericError("tool call '" + acc.name + "' was cut off by the output token cap");
        }
        call.arguments = acc.arguments;
      }
    }
    AppendToolCall(std::move(call), request);
  }
  pending_tools_.clear();
}

void OpenAIProtocol::ParseLine(std::string_view line, const ChatRequest& request) {
  if (line.empty()) {  // an SSE event ends at a blank line
    FlushPendingData(request);
    return;
  }
  if (line.front() == ':') return;  // a comment

  const std::size_t colon = line.find(':');
  if (colon == std::string_view::npos) return;
  const std::string_view field = line.substr(0, colon);
  std::string_view value = line.substr(colon + 1);
  // One optional space follows the colon; what is left is the value.
  if (!value.empty() && value.front() == ' ') value.remove_prefix(1);

  // `event`, `id` and `retry` name the event rather than carry it, so only `data` is read.
  if (field != "data") return;

  if (!pending_data_.empty()) pending_data_ += '\n';
  pending_data_.append(value);
  // The sentinel ends the stream on its own, and a payload split over several data lines
  // is whole once it parses — either way what follows starts a fresh one.
  if (pending_data_ == "[DONE]" || ParsesAsJson(pending_data_)) FlushPendingData(request);
}

void OpenAIProtocol::FlushPendingData(const ChatRequest& request) {
  if (pending_data_.empty()) return;

  std::string data = std::move(pending_data_);
  pending_data_.clear();

  if (data == "[DONE]") {
    boost::json::value done_obj = {{"done", true}};
    HandleJsonToken(done_obj, request);
    return;
  }

  boost::json::value j;
  try {
    j = boost::json::parse(data);
  } catch (const boost::system::system_error& e) {
    spdlog::warn("Skipping invalid JSON line: {}", e.what());
    return;
  }
  HandleJsonToken(j, request);
}

void OpenAIProtocol::FinishStream(const ChatRequest& request) {
  FlushPendingData(request);
  FlushPendingToolCalls(request);

  const auto seen = [this](std::string_view field) {
    return std::find(seen_fields_.begin(), seen_fields_.end(), field) != seen_fields_.end();
  };
  missing_fields_ = AbsentShapeFields(config_.shape, seen, /*required_only=*/true);

  // A refusal that arrived beside content is still the provider's own report on the
  // turn, so it follows the reply rather than being dropped with the empty-content case.
  if (!refusal_.empty()) {
    if (!content_.empty()) content_ += "\n";
    content_ += std::move(refusal_);
  }
}

}  // namespace pu
