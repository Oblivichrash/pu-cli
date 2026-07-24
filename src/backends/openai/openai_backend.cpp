// SPDX-License-Identifier: GPL-3.0-only
#include "backends/openai/openai_backend.hpp"

#include "backends/common/streaming_json_parser.hpp"
#include "infra/platform.hpp"
#include "pu/error.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <chrono>
#include <mutex>

namespace pu::backends::openai {

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

std::string RoleToString(pu::backend::Message::Role role) {
  switch (role) {
    case pu::backend::Message::Role::kSystem:    return "system";
    case pu::backend::Message::Role::kUser:      return "user";
    case pu::backend::Message::Role::kAssistant: return "assistant";
    case pu::backend::Message::Role::kTool:      return "tool";
    default: return "user";
  }
}

static json BuildMessagesJson(const std::vector<pu::backend::Message>& history) {
  json messages = json::array();
  for (const auto& msg : history) {
    json j{{"role", RoleToString(msg.role)}, {"content", msg.content}};
    if (msg.role == pu::backend::Message::Role::kTool) {
        j["tool_call_id"] = msg.tool_name;
    }
    if (!msg.tool_calls.empty()) {
        json tcs = json::array();
        for (const auto& tc : msg.tool_calls) {
            json func = {{"name", tc.name}};
            if (!tc.arguments.empty()) {
                func["arguments"] = tc.arguments;
            }
            tcs.push_back({{"id", tc.id}, {"type", "function"}, {"function", func}});
        }
        j["tool_calls"] = tcs;
    }
    messages.push_back(j);
  }
  return messages;
}

std::string SafeString(const json& j, const char* key) {
  return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : "";
}

}  // namespace

std::string OpenAIBackend::BuildRequest(const std::vector<pu::backend::Message>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;
  req["max_tokens"] = config_.max_tokens;

  auto messages_history = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const pu::backend::Message& m) {
        return m.role == pu::backend::Message::Role::kSystem;
      })) {
    messages_history.insert(messages_history.begin(),
        pu::backend::Message{pu::backend::Message::Role::kSystem, *config_.system_prompt});
  }

  req["messages"] = BuildMessagesJson(messages_history);
  return req.dump();
}

std::string OpenAIBackend::BuildRequestWithTools(
    const std::vector<pu::backend::Message>& history,
    const std::vector<pu::backend::ToolDefinition>& tools) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;
  req["max_tokens"] = config_.max_tokens;

  auto messages_history = history;
  if (config_.system_prompt && std::none_of(history.begin(), history.end(), [](const pu::backend::Message& m) {
        return m.role == pu::backend::Message::Role::kSystem;
      })) {
    messages_history.insert(messages_history.begin(),
        pu::backend::Message{pu::backend::Message::Role::kSystem, *config_.system_prompt});
  }

  req["messages"] = BuildMessagesJson(messages_history);

  json tools_json = json::array();
  for (const auto& tool : tools) {
    json params_json;
    try {
      params_json = json::parse(tool.parameters.raw_schema);
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

OpenAIBackend::OpenAIBackend(const Config& config, std::unique_ptr<pu::http::HttpClient> http)
    : Backend(config), http_(std::move(http)), host_(config.host),
      api_key_(config.api_key) {}

void OpenAIBackend::ResetAccumulators() { pending_tools_.clear(); }

void OpenAIBackend::HandleJsonToken(const json& j,
                                    backend::ChatCallback content_cb,
                                    backend::ToolCallback tool_cb) {
  bool is_final = false;
  if (j.contains("done") && j["done"].get<bool>()) is_final = true;

  if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
    const auto& delta = j["choices"][0].value("delta", json::object());
    if (delta.is_object()) {
      auto content = SafeString(delta, "content");
      if (!content.empty()) content_cb(backend::TokenType::kContent, content, false);
      auto reasoning = SafeString(delta, "reasoning_content");
      if (reasoning.empty()) reasoning = SafeString(delta, "reasoning");
      if (!reasoning.empty()) content_cb(backend::TokenType::kReasoning, reasoning, false);

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

  // Log token usage when present
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
      tool_cb({acc.id, acc.name, acc.arguments});
    }
    pending_tools_.clear();
    content_cb(backend::TokenType::kContent, "", true);
  }
}

void OpenAIBackend::Chat(const std::vector<pu::backend::Message>& history,
                         pu::backend::ChatCallback cb) {
  pu::platform::ClearInterruptFlag();
  ResetAccumulators();
  auto body = BuildRequest(history);
  std::string url = host_ + "/chat/completions";

  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  current_trace_id_ = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());

  StreamingJsonParser parser(
    [this, cb](std::string_view line) {
      constexpr std::string_view kDataPrefix = "data: ";
      auto start = line.find_first_not_of(" \t");
      if (start == std::string_view::npos) return;
      std::string_view trimmed = line.substr(start);
      if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) return;
      std::string_view data = trimmed.substr(kDataPrefix.size());
      if (data == "[DONE]") {
        cb(backend::TokenType::kContent, "", true);
        return;
      }
      try {
        auto j = json::parse(data);
        HandleJsonToken(j, cb, [](const backend::ToolCall&) {});
      } catch (const std::exception&) {}
    },
    [](const std::string& msg) { throw HttpError("OpenAI streaming error: " + msg); }
  );
  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };
  http_->PostStream(url, body, headers, write_cb);
}

void OpenAIBackend::Chat(const std::vector<pu::backend::Message>& history,
                         const std::vector<pu::backend::ToolDefinition>& tools,
                         pu::backend::ChatCallback content_cb,
                         pu::backend::ToolCallback tool_cb) {
  pu::platform::ClearInterruptFlag();
  ResetAccumulators();
  auto body = BuildRequestWithTools(history, tools);
  std::string url = host_ + "/chat/completions";

  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  current_trace_id_ = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());

  StreamingJsonParser parser(
    [this, content_cb, tool_cb](std::string_view line) {
      constexpr std::string_view kDataPrefix = "data: ";
      auto start = line.find_first_not_of(" \t");
      if (start == std::string_view::npos) return;
      std::string_view trimmed = line.substr(start);
      if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) return;
      std::string_view data = trimmed.substr(kDataPrefix.size());
      if (data == "[DONE]") {
        json done_obj;
        done_obj["done"] = true;
        HandleJsonToken(done_obj, content_cb, tool_cb);
        return;
      }
      try {
        auto j = json::parse(data);
        HandleJsonToken(j, content_cb, tool_cb);
      } catch (const std::exception&) {}
    },
    [](const std::string& msg) { throw HttpError("OpenAI streaming error: " + msg); }
  );
  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };
  http_->PostStream(url, body, headers, write_cb);
}

}  // namespace pu::backends::openai
