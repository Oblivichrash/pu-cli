// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/ollama_provider.hpp"

#include "pu/llm/projection.hpp"
#include "pu/llm/streaming_json_parser.hpp"
#include "pu/core/platform.hpp"
#include "pu/core/base.hpp"
#include "pu/core/json.hpp"

#include <boost/json.hpp>
#include <spdlog/spdlog.h>

namespace pu {

namespace {

// What this provider needs, as data: a closed set of roles, reasoning not echoed, content
// beside tool calls, arguments as an object.
constexpr llm::ProviderCapabilities kCapabilities{
    .role_naming = llm::RoleNaming::kKnownRolesOnly,
    .echo_reasoning_content = false,
    .allows_content_with_tool_calls = true,
    .tool_arguments = llm::ToolArgumentsEncoding::kJsonObject,
    .tool_calls_carry_type = false,
    .sends_tool_name = true,
    .omits_empty_tool_call_id = true,
};

}  // namespace

void OllamaProvider::ResetAccumulators() {
  content_.clear();
  current_reasoning_content_.clear();
  finish_reason_.clear();
  response_model_.clear();
  tool_calls_.clear();
  usage_.reset();
}

OllamaProvider::OllamaProvider(Config config, std::unique_ptr<pu::http::HttpClient> http)
    : StreamingProvider(config.host, config.api_key, std::move(http)),
      config_(std::move(config)) {}

std::string OllamaProvider::BuildRequest(const std::vector<ChatMessage>& history,
                                         const std::vector<ToolDefinition>& tools) const {
  boost::json::value req = {
      {"model", config_.model},
      {"stream", true},
      {"options", {{"temperature", config_.temperature}}},
      {"keep_alive", config_.keep_alive},
  };

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

void OllamaProvider::HandleJsonToken(const boost::json::value& j,
                                     std::function<void(const std::string&)>& content_cb) {
  // A failure arrives in place of a message: a stream that started is not a stream
  // that will finish, and the reason is in this frame.
  if (json::HasKey(j, "error")) {
    const std::string detail = json::ErrorMessage(j);
    if (!detail.empty()) throw Error("provider error: " + detail);
  }

  if (json::HasKey(j, "message")) {
    const auto& msg = j.at("message");
    if (json::HasKey(msg, "content") && msg.at("content").is_string()) {
      const std::string content = boost::json::value_to<std::string>(msg.at("content"));
      content_ += content;
      if (content_cb) content_cb(content);
    }

    // A thinking model reports its reasoning here rather than in `content`, under
    // a name of its own. Unread, it is generated and then thrown away.
    if (json::HasKey(msg, "thinking") && msg.at("thinking").is_string()) {
      EmitReasoning(boost::json::value_to<std::string>(msg.at("thinking")));
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
            try {
              call.arguments = boost::json::parse(boost::json::value_to<std::string>(args));
            } catch (const std::exception& e) {
              // Preserve the raw value when arguments are not valid JSON
              // instead of dropping the tool call.
              spdlog::debug("Keeping non-JSON tool arguments as-is: {}", e.what());
              call.arguments = args;
            }
          } else if (args.is_object() || args.is_array()) {
            call.arguments = args;
          }
        }
        tool_calls_.push_back(std::move(call));
      }
    }
  }

  // Ollama reports what the request cost in the final object, beside `done`.
  if (json::HasKey(j, "prompt_eval_count") || json::HasKey(j, "eval_count")) {
    usage_ = TokenUsage{json::ValueOrDefault<int>(j, "prompt_eval_count", 0),
                        json::ValueOrDefault<int>(j, "eval_count", 0)};
  }

  // Why it stopped, in the same final object. The word it uses for the token limit
  // is the one OpenAI uses, which is what lets a caller read both.
  if (json::HasKey(j, "done_reason") && j.at("done_reason").is_string()) {
    finish_reason_ = boost::json::value_to<std::string>(j.at("done_reason"));
  }

  // The model that answered: a tag can resolve to a different build than the one
  // that was asked for.
  if (json::HasKey(j, "model") && j.at("model").is_string()) {
    response_model_ = boost::json::value_to<std::string>(j.at("model"));
  }
}

void OllamaProvider::ParseLine(std::string_view line,
                               std::function<void(const std::string&)>& content_cb) {
  // Only the parse is guarded: a frame filled with an error is a valid parse and has to
  // reach the caller.
  boost::json::value j;
  try {
    j = boost::json::parse(line);
  } catch (const boost::system::system_error& e) {
    // Skip lines with incomplete/invalid UTF-8 instead of failing the stream.
    spdlog::warn("Skipping invalid JSON line (UTF-8 error): {}", e.what());
    return;
  } catch (const std::exception&) {
    return;
  }
  HandleJsonToken(j, content_cb);
}

}  // namespace pu
