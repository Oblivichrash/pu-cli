// SPDX-License-Identifier: GPL-3.0-only
#include "backends/ollama/ollama_token_adapter.hpp"
#include <nlohmann/json.hpp>

namespace pu::backends::ollama {

void OllamaTokenAdapter::HandleJson(const nlohmann::json& j,
                                    backend::ChatCallback content_cb,
                                    backend::ToolCallback tool_cb) {
  if (j.contains("message")) {
    const auto& msg = j["message"];
    if (msg.contains("content") && msg["content"].is_string())
      content_cb(backend::TokenType::kContent, msg["content"].get<std::string>(), false);
    if (msg.contains("thinking") && msg["thinking"].is_string())
      content_cb(backend::TokenType::kReasoning, msg["thinking"].get<std::string>(), false);
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      for (const auto& tc : msg["tool_calls"]) {
        backend::ToolCall call;
        if (tc.contains("id")) call.id = tc["id"].get<std::string>();
        if (tc.contains("function")) {
          call.name = tc["function"].value("name", "");
          if (tc["function"].contains("arguments")) {
            const auto& args = tc["function"]["arguments"];
            if (args.is_string()) call.arguments = args.get<std::string>();
            else if (args.is_object() || args.is_array()) call.arguments = args.dump();
          }
        }
        tool_cb(call);
      }
    }
  }

  if (j.contains("done") && j["done"].get<bool>()) {
    content_cb(backend::TokenType::kContent, "", true);
  }
}

}  // namespace pu::backends::ollama
