// SPDX-License-Identifier: GPL-3.0-only
#include "openai.hpp"
#include "core/system.hpp"
#include "backends/common/streaming_json_parser.hpp"
#include <nlohmann/json.hpp>

namespace pu::backends::openai {

using json = nlohmann::json;

static std::string SafeString(const json& j, const char* key) {
  return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : "";
}

static std::string RoleToString(pu::backend::Message::Role role) {
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
    if (msg.role == pu::backend::Message::Role::kTool) j["tool_call_id"] = msg.tool_name;
    if (!msg.tool_calls.empty()) {
      json tcs = json::array();
      for (const auto& tc : msg.tool_calls) {
        json func = {{"name", tc.name}};
        if (!tc.arguments.empty()) {
          try { func["arguments"] = json::parse(tc.arguments); }
          catch (...) { func["arguments"] = tc.arguments; }
        }
        tcs.push_back({{"id", tc.id}, {"type", "function"}, {"function", func}});
      }
      j["tool_calls"] = tcs;
    }
    messages.push_back(j);
  }
  return messages;
}

OpenAIBackend::OpenAIBackend(const Config& config, std::unique_ptr<pu::http::HttpClient> http)
    : Backend(config), http_(std::move(http)), host_(config.host), api_key_(config.api_key) {}

std::string OpenAIBackend::BuildRequest(const std::vector<pu::backend::Message>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;
  req["messages"] = BuildMessagesJson(InjectSystemPrompt(history, config_.system_prompt));
  return req.dump();
}

std::string OpenAIBackend::BuildRequestWithTools(
    const std::vector<pu::backend::Message>& history,
    const std::vector<pu::backend::ToolDefinition>& tools) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;
  req["messages"] = BuildMessagesJson(InjectSystemPrompt(history, config_.system_prompt));

  json tools_json = json::array();
  for (const auto& tool : tools) {
    tools_json.push_back({
      {"type", "function"},
      {"function", {
        {"name", tool.name},
        {"description", tool.description},
        {"parameters", json::parse(tool.parameters.raw_schema)}
      }}
    });
  }
  req["tools"] = tools_json;
  return req.dump();
}

void OpenAIBackend::HandleJsonChunk(const json& j,
                                    pu::backend::ChatCallback content_cb,
                                    pu::backend::ToolCallback tool_cb) {
  bool is_final = false;
  if (j.contains("done") && j["done"].get<bool>()) is_final = true;

  if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
    const auto& delta = j["choices"][0].value("delta", json::object());
    if (delta.is_object()) {
      auto content = SafeString(delta, "content");
      if (!content.empty()) content_cb(pu::backend::TokenType::kContent, content, false);
      auto reasoning = SafeString(delta, "reasoning_content");
      if (reasoning.empty()) reasoning = SafeString(delta, "reasoning");
      if (!reasoning.empty()) content_cb(pu::backend::TokenType::kReasoning, reasoning, false);

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

  if (is_final) {
    for (auto& [idx, acc] : pending_tools_) {
      tool_cb({acc.id, acc.name, acc.arguments});
    }
    pending_tools_.clear();
    content_cb(pu::backend::TokenType::kContent, "", true);
  }
}

void OpenAIBackend::Chat(const std::vector<pu::backend::Message>& history,
                         pu::backend::ChatCallback cb, std::error_code& ec) {
  pu::platform::ClearInterruptFlag();
  pending_tools_.clear();
  auto body = BuildRequest(history);
  std::string url = host_ + "/chat/completions";

  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  StreamingJsonParser parser(
    [this, cb](std::string_view line) {
      constexpr std::string_view kDataPrefix = "data: ";
      auto start = line.find_first_not_of(" \t");
      if (start == std::string_view::npos) return;
      std::string_view trimmed = line.substr(start);
      if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) return;
      std::string_view data = trimmed.substr(kDataPrefix.size());
      if (data == "[DONE]") {
        cb(pu::backend::TokenType::kContent, "", true);
        return;
      }
      try {
        auto j = json::parse(data);
        HandleJsonChunk(j, cb, [](const pu::backend::ToolCall&) {});
      } catch (const std::exception&) {}
    },
    [&ec](std::error_code err) { if (!ec) ec = err; }
  );
  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };
  http_->PostStream(url, body, headers, write_cb, ec);
}

void OpenAIBackend::Chat(const std::vector<pu::backend::Message>& history,
                         const std::vector<pu::backend::ToolDefinition>& tools,
                         pu::backend::ChatCallback content_cb,
                         pu::backend::ToolCallback tool_cb,
                         std::error_code& ec) {
  pu::platform::ClearInterruptFlag();
  pending_tools_.clear();
  auto body = BuildRequestWithTools(history, tools);
  std::string url = host_ + "/chat/completions";

  std::vector<std::string> headers = {"Content-Type: application/json"};
  if (!api_key_.empty()) headers.push_back("Authorization: Bearer " + api_key_);

  StreamingJsonParser parser(
    [this, content_cb, tool_cb](std::string_view line) {
      constexpr std::string_view kDataPrefix = "data: ";
      auto start = line.find_first_not_of(" \t");
      if (start == std::string_view::npos) return;
      std::string_view trimmed = line.substr(start);
      if (trimmed.substr(0, kDataPrefix.size()) != kDataPrefix) return;
      std::string_view data = trimmed.substr(kDataPrefix.size());
      if (data == "[DONE]") {
        HandleJsonChunk({{"done", true}}, content_cb, tool_cb);
        return;
      }
      try {
        auto j = json::parse(data);
        HandleJsonChunk(j, content_cb, tool_cb);
      } catch (const std::exception&) {}
    },
    [&ec](std::error_code err) { if (!ec) ec = err; }
  );
  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    parser.Feed(ptr, total);
    if (pu::platform::IsInterrupted()) return 0;
    return total;
  };
  http_->PostStream(url, body, headers, write_cb, ec);
}

}  // namespace pu::backends::openai
