// SPDX-License-Identifier: GPL-3.0-only
#include "request_builder.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <optional>

namespace pu::backends {

using json = nlohmann::json;

std::string RequestBuilder::RoleToString(pu::backend::Message::Role role) {
  switch (role) {
    case pu::backend::Message::Role::kSystem:    return "system";
    case pu::backend::Message::Role::kUser:      return "user";
    case pu::backend::Message::Role::kAssistant: return "assistant";
    case pu::backend::Message::Role::kTool:      return "tool";
    default: return "user";
  }
}

json RequestBuilder::BuildMessagesJson(const std::vector<pu::backend::Message>& history) {
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

json RequestBuilder::BuildMessagesJsonOllama(const std::vector<pu::backend::Message>& history) {
  json messages = json::array();
  for (const auto& msg : history) {
    std::string role;
    switch (msg.role) {
      case pu::backend::Message::Role::kSystem:    role = "system"; break;
      case pu::backend::Message::Role::kUser:      role = "user"; break;
      case pu::backend::Message::Role::kAssistant: role = "assistant"; break;
      case pu::backend::Message::Role::kTool:      role = "tool"; break;
    }
    json m = {{"role", role}, {"content", msg.content}};
    if (role == "tool") m["tool_name"] = msg.tool_name;
    if (!msg.tool_calls.empty()) {
      json tcs = json::array();
      for (const auto& tc : msg.tool_calls) {
        json func = {{"name", tc.name}};
        if (!tc.arguments.empty()) {
          try { func["arguments"] = json::parse(tc.arguments); }
          catch (...) { func["arguments"] = tc.arguments; }
        }
        tcs.push_back({{"function", func}});
      }
      m["tool_calls"] = tcs;
    }
    messages.push_back(m);
  }
  return messages;
}

json RequestBuilder::BuildToolsJsonOllama(const std::vector<pu::backend::ToolDefinition>& tools) {
  json tools_json = json::array();
  for (const auto& tool : tools) {
    json t;
    t["type"] = "function";
    t["function"]["name"] = tool.name;
    t["function"]["description"] = tool.description;
    try {
      t["function"]["parameters"] = json::parse(tool.parameters.raw_schema);
    } catch (const std::exception& e) {
      std::cerr << "[OllamaBackend] Failed to parse schema for tool '" << tool.name
                << "': " << e.what() << std::endl;
      continue;
    }
    tools_json.push_back(t);
  }
  return tools_json;
}

json RequestBuilder::BuildToolsJsonOpenAI(const std::vector<pu::backend::ToolDefinition>& tools) {
  json tools_json = json::array();
  for (const auto& tool : tools) {
    try {
      tools_json.push_back({
        {"type", "function"},
        {"function", {
          {"name", tool.name},
          {"description", tool.description},
          {"parameters", json::parse(tool.parameters.raw_schema)}
        }}
      });
    } catch (const std::exception& e) {
      std::cerr << "[OpenAIBackend] Failed to parse schema for tool '" << tool.name
                << "': " << e.what() << std::endl;
    }
  }
  return tools_json;
}

json RequestBuilder::BuildChatRequest(
    BackendFlavor flavor,
    const std::string& model,
    float temperature,
    const std::vector<pu::backend::Message>& history,
    const std::optional<std::string>& system_prompt)
{
  json req;
  req["model"] = model;
  req["stream"] = true;

  if (flavor == BackendFlavor::kOllama) {
    req["options"]["temperature"] = temperature;
  } else {
    req["temperature"] = temperature;
  }

  auto messages_history = pu::backend::Backend::InjectSystemPrompt(history, system_prompt);

  if (flavor == BackendFlavor::kOllama) {
    req["messages"] = BuildMessagesJsonOllama(messages_history);
  } else {
    req["messages"] = BuildMessagesJson(messages_history);
  }

  return req;
}

json RequestBuilder::BuildChatRequestWithTools(
    BackendFlavor flavor,
    const std::string& model,
    float temperature,
    const std::vector<pu::backend::Message>& history,
    const std::optional<std::string>& system_prompt,
    const std::vector<pu::backend::ToolDefinition>& tools)
{
  json req;
  req["model"] = model;
  req["stream"] = true;

  if (flavor == BackendFlavor::kOllama) {
    req["options"]["temperature"] = temperature;
  } else {
    req["temperature"] = temperature;
  }

  auto messages_history = pu::backend::Backend::InjectSystemPrompt(history, system_prompt);

  if (flavor == BackendFlavor::kOllama) {
    req["messages"] = BuildMessagesJsonOllama(messages_history);
    req["tools"] = BuildToolsJsonOllama(tools);
  } else {
    req["messages"] = BuildMessagesJson(messages_history);
    req["tools"] = BuildToolsJsonOpenAI(tools);
  }

  return req;
}

} // namespace pu::backends
