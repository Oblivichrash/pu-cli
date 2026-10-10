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

void OllamaProvider::ResetAccumulators() {
  content_.clear();
  reasoning_.clear();
  finish_reason_.clear();
  response_model_.clear();
  tool_calls_.clear();
  usage_.reset();
}

OllamaProvider::OllamaProvider(Config config, std::shared_ptr<pu::http::HttpClient> http)
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

void OllamaProvider::HandleJsonToken(const boost::json::value& j, const ChatRequest& request) {
  if (json::HasKey(j, "error")) {
    const std::string detail = json::ErrorMessage(j);
    if (!detail.empty()) throw Error("provider error: " + detail);
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
            try {
              call.arguments = boost::json::parse(boost::json::value_to<std::string>(args));
            } catch (const std::exception& e) {
              spdlog::debug("Keeping non-JSON tool arguments as-is: {}", e.what());
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
    usage_ = TokenUsage{json::ValueOrDefault<int>(j, "prompt_eval_count", 0),
                        json::ValueOrDefault<int>(j, "eval_count", 0)};
  }

  if (json::HasKey(j, "done_reason") && j.at("done_reason").is_string()) {
    finish_reason_ = boost::json::value_to<std::string>(j.at("done_reason"));
  }

  if (json::HasKey(j, "model") && j.at("model").is_string()) {
    response_model_ = boost::json::value_to<std::string>(j.at("model"));
  }
}

void OllamaProvider::ParseLine(std::string_view line, const ChatRequest& request) {
  boost::json::value j;
  try {
    j = boost::json::parse(line);
  } catch (const boost::system::system_error& e) {
    spdlog::warn("Skipping invalid JSON line: {}", e.what());
    return;
  }
  HandleJsonToken(j, request);
}

}  // namespace pu
