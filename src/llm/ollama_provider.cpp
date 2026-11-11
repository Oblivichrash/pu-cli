// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/ollama_provider.hpp"

#include "pu/llm/projection.hpp"
#include "pu/core/platform.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"
#include "pu/core/text.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

namespace pu {

namespace {

constexpr llm::ProviderCapabilities kCapabilities{
    .role_naming = llm::RoleNaming::kKnownRolesOnly,
    .echoes_reasoning = true,
    .reasoning_field = "thinking",
    .allows_content_with_tool_calls = true,
    .tool_arguments = llm::ToolArgumentsEncoding::kJsonObject,
    .tool_calls_carry_type = false,
    .sends_tool_name = true,
    .omits_empty_tool_call_id = true,
};

}  // namespace

OllamaProvider::OllamaProvider(Config config, std::shared_ptr<pu::http::HttpClient> http)
    : StreamingProvider(config.host, config.api_key, std::move(http)),
      config_(std::move(config)) {}

std::string OllamaProvider::BuildRequest(std::span<const ChatMessage> history,
                                         std::span<const ToolDefinition> tools) const {
  boost::json::value req = {
      {"model", config_.model},
      {"stream", true},
      {"options", {{"temperature", config_.temperature}}},
      {"keep_alive", config_.keep_alive},
  };

  if (config_.thinking != ThinkingLevel::kServerDefault) {
    req.as_object()["think"] = llm::OllamaThinkValue(config_.thinking);
  }

  req.as_object()["messages"] = llm::ProjectMessages(history, kCapabilities);

  if (!tools.empty()) {
    boost::json::array tools_json;
    for (const auto& tool : tools) {
      tools_json.push_back(boost::json::value{{"type", "function"},
                                              {"function",
                                               {
                                                   {"name", tool.name},
                                                   {"description", tool.description},
                                                   {"parameters", tool.Parameters()},
                                               }}});
    }
    req.as_object()["tools"] = tools_json;
  }
  return boost::json::serialize(req);
}

void OllamaProvider::HandleJsonToken(const boost::json::value& j, const ChatRequest& request) {
  // The top-level names this reads, kept beside the reads themselves so a name added to
  // the wire has to pass them to go unnoticed.
  static constexpr std::string_view kReadKeys[] = {
      "error", "message", "done", "done_reason", "model",
      "prompt_eval_count", "eval_count", "prompt_eval_cached_count",
  };
  NoteUnreadFields(j, kReadKeys);

  if (json::HasKey(j, "error")) {
    const std::string detail = json::ErrorMessage(j);
    if (!detail.empty()) throw GenericError("provider error: " + detail);
  }

  if (json::HasKey(j, "message")) {
    const auto& msg = j.at("message");
    if (json::HasKey(msg, "content") && msg.at("content").is_string()) {
      AppendContent(boost::json::value_to<std::string>(msg.at("content")), request);
    }

    if (json::HasKey(msg, "thinking") && msg.at("thinking").is_string()) {
      AppendReasoning(boost::json::value_to<std::string>(msg.at("thinking")), request);
    }

    if (json::HasKey(msg, "tool_calls") && msg.at("tool_calls").is_array()) {
      for (const auto& tc : msg.at("tool_calls").as_array()) {
        if (!json::HasKey(tc, "function")) continue;
        std::string tool_name = json::ValueOrDefault<std::string>(tc.at("function"), "name", "");
        if (tool_name.empty()) continue;

        ToolCall call;
        if (json::HasKey(tc, "id") && tc.at("id").is_string()) {
          call.id = boost::json::value_to<std::string>(tc.at("id"));
        }
        call.name = tool_name;
        if (json::HasKey(tc.at("function"), "arguments")) {
          const auto& args = tc.at("function").at("arguments");
          if (args.is_string()) {
            std::optional<boost::json::value> parsed =
                json::TryParse(boost::json::value_to<std::string>(args));
            if (parsed) {
              call.arguments = std::move(*parsed);
            } else {
              spdlog::debug("Keeping non-JSON tool arguments as-is");
              call.arguments = args;
            }
          } else if (args.is_object() || args.is_array()) {
            call.arguments = args;
          }
        }
        AppendToolCall(std::move(call), request);
      }
    }
  }

  if (json::HasKey(j, "prompt_eval_count") || json::HasKey(j, "eval_count")) {
    // Named rather than positional: `TokenUsage` carries more than the two counts a daemon
    // reports, and a positional list would have to grow with it.
    acc_.usage = TokenUsage{};
    acc_.usage->prompt_tokens = json::ValueOrDefault<int>(j, "prompt_eval_count", 0);
    acc_.usage->completion_tokens = json::ValueOrDefault<int>(j, "eval_count", 0);
    // The cached prompt rides on the same line under Ollama's own name; without it the
    // count reads as a gateway that cached nothing.
    acc_.usage->cached_tokens = json::ValueOrDefault<int>(j, "prompt_eval_cached_count", 0);
  }

  // Ollama reports no usage object: the counts and durations ride on the final line,
  // which is kept whole rather than filtered to a list of names -- a list is a name
  // someone has to remember to add to, and `prompt_eval_cached_count` was missed by one.
  if (json::HasKey(j, "prompt_eval_count") || json::HasKey(j, "eval_count")) {
    acc_.raw_usage = j;
  }

  if (json::HasKey(j, "done_reason") && j.at("done_reason").is_string()) {
    acc_.finish_reason = boost::json::value_to<std::string>(j.at("done_reason"));
  }

  if (json::HasKey(j, "model") && j.at("model").is_string()) {
    acc_.response_model = boost::json::value_to<std::string>(j.at("model"));
  }
}

void OllamaProvider::ParseData(std::string_view data, const ChatRequest& request) {
  // NDJSON frames nothing: a blank line is a blank line, and the frame is the payload the
  // parser handed over.
  if (text::Trim(data).empty()) return;

  const std::optional<boost::json::value> j = json::TryParse(data);
  if (!j) {
    // One lost line is one lost line: a daemon that writes a partial line at shutdown still
    // delivered every frame before it.
    spdlog::warn("Skipping invalid JSON line");
    return;
  }
  HandleJsonToken(*j, request);
}

}  // namespace pu
