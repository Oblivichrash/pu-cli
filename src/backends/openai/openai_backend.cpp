// SPDX-License-Identifier: GPL-3.0-only

#include "openai_backend.hpp"
#include "sse_parser.hpp"
#include <nlohmann/json.hpp>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace pu::backends::openai {

using json = nlohmann::json;

namespace {

std::string RoleToString(pu::backend::Message::Role role) {
  switch (role) {
    case pu::backend::Message::Role::kSystem: return "system";
    case pu::backend::Message::Role::kUser: return "user";
    case pu::backend::Message::Role::kAssistant: return "assistant";
    case pu::backend::Message::Role::kTool: return "tool";
    default: return "user";
  }
}

json BuildMessagesJson(const std::vector<pu::backend::Message>& history) {
  json messages = json::array();
  for (const auto& msg : history) {
    json j{
      {"role", RoleToString(msg.role)},
      {"content", msg.content}
    };
    if (msg.role == pu::backend::Message::Role::kTool) {
      j["tool_call_id"] = msg.tool_name;
    }
    if (!msg.tool_calls.empty()) {
      json tcs = json::array();
      for (const auto& tc : msg.tool_calls) {
        json func = {{"name", tc.name}};
        if (!tc.arguments.empty()) {
          try {
            func["arguments"] = json::parse(tc.arguments);
          } catch (...) {
            func["arguments"] = tc.arguments;
          }
        }
        tcs.push_back({
          {"id", tc.id},
          {"type", "function"},
          {"function", func}
        });
      }
      j["tool_calls"] = tcs;
    }
    messages.push_back(j);
  }
  return messages;
}

}  // namespace

std::string OpenAIBackend::BuildRequest(const std::vector<pu::backend::Message>& history) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;
  req["messages"] = BuildMessagesJson(BuildMessagesWithSystemPrompt(history));
  return req.dump();
}

std::string OpenAIBackend::BuildRequestWithTools(
    const std::vector<pu::backend::Message>& history,
    const std::vector<pu::backend::ToolDefinition>& tools) const {
  json req;
  req["model"] = config_.model;
  req["stream"] = true;
  req["temperature"] = config_.temperature;
  req["messages"] = BuildMessagesJson(BuildMessagesWithSystemPrompt(history));

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

OpenAIBackend::OpenAIBackend(const Config& config,
                             std::unique_ptr<pu::http::HttpClient> http)
    : Backend(config),
      http_(std::move(http)),
      host_(config.host),
      api_key_(config.api_key) {}

void OpenAIBackend::Chat(const std::vector<pu::backend::Message>& history,
                         pu::backend::ChatCallback cb) {
  std::string body = BuildRequest(history);
  std::string url = host_ + "/chat/completions";

  std::vector<std::string> headers;
  headers.push_back("Content-Type: application/json");
  if (!api_key_.empty()) {
    headers.push_back("Authorization: Bearer " + api_key_);
  }

  std::string line_buffer;
  std::string accumulated_content;
  std::string accumulated_reasoning;

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    line_buffer.append(ptr, total);
    size_t pos = 0;
    while ((pos = line_buffer.find('\n')) != std::string::npos) {
      std::string line = line_buffer.substr(0, pos);
      line_buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();

      auto token_opt = internal::ParseSseLine(line);
      if (!token_opt) continue;

      const auto& token = *token_opt;
      if (token.done) {
        cb(pu::backend::TokenType::kContent, "", true);
        return total;
      }

      auto extract_delta = [](std::string_view new_text, std::string& accumulated) -> std::string_view {
        if (new_text.empty()) return new_text;
        if (new_text.size() < accumulated.size()) accumulated.clear();
        if (accumulated.empty() ||
            new_text.compare(0, accumulated.size(), accumulated) != 0) {
          accumulated = new_text;
          return new_text;
        }
        std::string_view delta = new_text.substr(accumulated.size());
        accumulated = new_text;
        return delta;
      };

      if (!token.reasoning.empty()) {
        std::string_view delta = extract_delta(token.reasoning, accumulated_reasoning);
        if (!delta.empty()) cb(pu::backend::TokenType::kReasoning, delta, false);
      }
      if (!token.content.empty()) {
        std::string_view delta = extract_delta(token.content, accumulated_content);
        if (!delta.empty()) cb(pu::backend::TokenType::kContent, delta, false);
      }
    }
    return total;
  };

  http_->PostStream(url, body, headers, write_cb);
}

void OpenAIBackend::Chat(const std::vector<pu::backend::Message>& history,
                         const std::vector<pu::backend::ToolDefinition>& tools,
                         pu::backend::ChatCallback content_cb,
                         pu::backend::ToolCallback tool_cb) {
  std::string body = BuildRequestWithTools(history, tools);
  std::string url = host_ + "/chat/completions";

  std::vector<std::string> headers;
  headers.push_back("Content-Type: application/json");
  if (!api_key_.empty()) {
    headers.push_back("Authorization: Bearer " + api_key_);
  }

  std::string line_buffer;
  std::string accumulated_content;
  std::string accumulated_reasoning;
  std::map<int, internal::ToolCallDelta> pending_tools;

  auto write_cb = [&](char* ptr, size_t total) -> size_t {
    line_buffer.append(ptr, total);
    size_t pos = 0;
    while ((pos = line_buffer.find('\n')) != std::string::npos) {
      std::string line = line_buffer.substr(0, pos);
      line_buffer.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();

      auto token_opt = internal::ParseSseLine(line);
      if (!token_opt) continue;

      const auto& token = *token_opt;
      if (token.done) {
        for (auto& [idx, delta] : pending_tools) {
          pu::backend::ToolCall call;
          call.id = delta.id;
          call.name = delta.name;
          call.arguments = delta.arguments;
          tool_cb(call);
        }
        pending_tools.clear();
        content_cb(pu::backend::TokenType::kContent, "", true);
        return total;
      }

      if (!token.content.empty()) {
        content_cb(pu::backend::TokenType::kContent, token.content, false);
      }
      if (!token.reasoning.empty()) {
        content_cb(pu::backend::TokenType::kReasoning, token.reasoning, false);
      }
      for (const auto& delta : token.tool_call_deltas) {
        if (delta.index < 0) continue;
        auto& acc = pending_tools[delta.index];
        if (!delta.id.empty()) acc.id = delta.id;
        if (!delta.name.empty()) acc.name = delta.name;
        acc.arguments += delta.arguments;
      }
    }
    return total;
  };

  http_->PostStream(url, body, headers, write_cb);
}

}  // namespace pu::backends::openai
