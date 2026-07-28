// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/providers/openai_provider.hpp"

#include "pu/llm/common/streaming_json_parser.hpp"
#include "pu/infra/platform.hpp"
#include "pu/error.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>
#include <mutex>
#include <sstream>

namespace pu {

using json = nlohmann::json;

namespace {

static bool IsTraceEnabled() {
  static const char* env = std::getenv("PU_TRACE");
  return env && (std::string(env) == "1" || std::string(env) == "true");
}

static std::ofstream& GetTraceLog() {
  static std::ofstream log;
  static std::once_flag flag;
  std::call_once(flag, []() {
    const char* path = std::getenv("PU_TRACE_LOG");
    if (!path) path = "/tmp/pu_trace.jsonl";
    log.open(path, std::ios::app);
  });
  return log;
}

std::string SafeString(const json& j, const char* key) {
  return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : "";
}

json BuildMessagesJson(const std::vector<ChatMessage>& history) {
  json messages = json::array();
  for (const auto& msg : history) {
    std::string role = msg.role;
    if (role == "tool_result") role = "tool";

    json j{{"role", role}, {"content", msg.content}};
    if (role == "tool") {
      j["tool_call_id"] = msg.tool_name;
    }
    if (!msg.tool_calls_json.empty()) {
      try {
        j["tool_calls"] = json::parse(msg.tool_calls_json);
      } catch (const std::exception&) {}
    }
    messages.push_back(j);
  }
  return messages;
}

}  // namespace

OpenAIProvider::OpenAIProvider(const Config& config,
                               std::unique_ptr<pu::http::HttpClient> http)
    : config_(config), http_(std::move(http)),
      host_(config_.host), api_key_(config_.api_key) {}

void OpenAIProvider::ResetAccumulators() {
  pending_tools_.clear();
}

std::string OpenAIProvider::BuildRequest(const std::vector<ChatMessage>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;
  req["max_tokens"] = config_.max_tokens;

  auto messages = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const ChatMessage& m) {
        return m.role == "system";
      })) {
    ChatMessage sys;
    sys.role = "system";
    sys.content = *config_.system_prompt;
    messages.insert(messages.begin(), std::move(sys));
  }

  req["messages"] = BuildMessagesJson(messages);
  return req.dump();
}

std::string OpenAIProvider::BuildRequestWithTools(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;
  req["max_tokens"] = config_.max_tokens;

  auto messages = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const ChatMessage& m) {
        return m.role == "system";
      })) {
    ChatMessage sys;
    sys.role = "system";
    sys.content = *config_.system_prompt;
    messages.insert(messages.begin(), std::move(sys));
  }

  req["messages"] = BuildMessagesJson(messages);

  json tools_json = json::array();
  for (const auto& tool : tools) {
    json params_json;
    try {
      params_json = json::parse(tool.parameters_schema);
    } catch (const std::exception&) {
      params_json = json::object();
    }

    json function_obj = {
      {"name", tool.name},
      {"description", tool.description}
    };

    if (config_.parameters_as_string) {
      function_obj["parameters"] = params_json.dump();
    } else {
      function_obj["parameters"] = params_json;
    }

    tools_json.push_back({{"type", "function"}, {"function", function_obj}});
  }
  req["tools"] = tools_json;
  return req.dump();
}

void OpenAIProvider::HandleJsonToken(const json& j,
                                     std::function<void(const std::string&)>& content_cb,
                                     std::function<void(const ToolCall&)>& tool_cb) {
  bool is_final = false;
  if (j.contains("done") && j["done"].get<bool>()) is_final = true;

  if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
    const auto& delta = j["choices"][0].value("delta", json::object());
    if (delta.is_object()) {
      auto content = SafeString(delta, "content");
      if (!content.empty() && content_cb) content_cb(content);
      // Ignore reasoning_content and reasoning fields - reasoning content is not forwarded

      if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc : delta["tool_calls"]) {
          if (!tc.is_object()) continue;
          int idx = tc.value("index", -1);
          if (idx < 0) continue;
          auto& acc = pending_tools_[idx];
          auto id = SafeString(tc, "id");
          if (!id.empty()) acc.id = id;
          if (tc.contains("function") && tc["function"].is_object()) {
            auto name = SafeString(tc["function"], "name");
            if (!name.empty()) acc.name = name;
            acc.arguments += SafeString(tc["function"], "arguments");
          }
        }
      }
    }
  }

  if (j.contains("usage") && j["usage"].is_object()) {
    if (IsTraceEnabled()) {
      auto usage = j["usage"];
      nlohmann::json usage_log;
      usage_log["trace_id"] = current_trace_id_;
      usage_log["model"] = config_.model;
      usage_log["input_tokens"] = usage.value("prompt_tokens", 0);
      usage_log["output_tokens"] = usage.value("completion_tokens", 0);
      usage_log["total_tokens"] = usage.value("total_tokens", 0);
      GetTraceLog() << usage_log.dump() << std::endl;
    }
  }

  if (is_final) {
    for (auto& [idx, acc] : pending_tools_) {
      ToolCall call;
      call.name = acc.name;
      if (!acc.arguments.empty()) {
        try {
          call.arguments = json::parse(acc.arguments);
        } catch (const std::exception&) {
          call.arguments = acc.arguments;
        }
      }
      if (tool_cb) tool_cb(call);
    }
    pending_tools_.clear();
  }
}

ChatResult OpenAIProvider::Chat(
    const std::vector<ChatMessage>& history,
    const std::vector<ToolDefinition>& tools,
    std::function<void(const std::string&)> content_callback,
    std::function<void(const ToolCall&)> tool_callback) {
  ChatResult result;
  platform::ClearInterruptFlag();
  ResetAccumulators();

  std::string body;
  if (tools.empty()) {
    body = BuildRequest(history);
  } else {
    body = BuildRequestWithTools(history, tools);
  }

  std::string url = host_ + "/chat/completions";
  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  current_trace_id_ = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());

  std::ostringstream content_stream;
  std::vector<ToolCall> collected_calls;

  llm::StreamingJsonParser parser(
    [&](std::string_view line) {
      constexpr std::string_view kDataPrefix = "data: ";
      auto start = line.find_first_not_of(" \t");
      if (start == std::string_view::npos) return;
      std::string_view trimmed = line.substr(start);
      if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) return;
      std::string_view data = trimmed.substr(kDataPrefix.size());
      if (data == "[DONE]") {
        json done_obj;
        done_obj["done"] = true;
        HandleJsonToken(done_obj, content_callback, tool_callback);
        return;
      }
      try {
        auto j = json::parse(data);
        HandleJsonToken(j, content_callback, tool_callback);

        // Collect content for result
        if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
          const auto& delta = j["choices"][0].value("delta", json::object());
          if (delta.is_object()) {
            auto content = SafeString(delta, "content");
            content_stream << content;
          }
        }
      } catch (const std::exception&) {}
    },
    [](const std::string& msg) { throw HttpError("OpenAI streaming error: " + msg); }
  );

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (platform::IsInterrupted()) return 0;
    return total;
  };

  http_->PostStream(url, body, headers, write_cb);

  result.content = content_stream.str();
  result.tool_calls = std::move(collected_calls);
  return result;
}

}  // namespace pu