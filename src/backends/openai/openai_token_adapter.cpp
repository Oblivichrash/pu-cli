// SPDX-License-Identifier: GPL-3.0-only

#include "openai_token_adapter.hpp"
#include <nlohmann/json.hpp>

namespace pu::backends::openai {

static std::string SafeString(const nlohmann::json& j, const char* key) {
  if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
  return "";
}

void OpenAITokenAdapter::HandleJson(const nlohmann::json& j,
                                    backend::ChatCallback content_cb,
                                    backend::ToolCallback tool_cb) {
  bool is_final = false;
  if (j.contains("done") && j["done"].get<bool>()) {
    is_final = true;
  }

  if (j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
    const auto& choice = j["choices"][0];
    if (choice.contains("delta") && choice["delta"].is_object()) {
      const auto& delta = choice["delta"];

      std::string content = SafeString(delta, "content");
      if (!content.empty()) {
        content_cb(backend::TokenType::kContent, content, false);
      }

      std::string reasoning = SafeString(delta, "reasoning_content");
      if (reasoning.empty()) reasoning = SafeString(delta, "reasoning");
      if (!reasoning.empty()) {
        content_cb(backend::TokenType::kReasoning, reasoning, false);
      }

      if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& tc : delta["tool_calls"]) {
          if (!tc.is_object()) continue;
          int idx = tc.value("index", -1);
          if (idx < 0) continue;
          auto& acc = pending_tools_[idx];
          std::string id_str = SafeString(tc, "id");
          if (!id_str.empty()) acc.id = id_str;
          if (tc.contains("function") && tc["function"].is_object()) {
            std::string name = SafeString(tc["function"], "name");
            if (!name.empty()) acc.name = name;
            acc.arguments += SafeString(tc["function"], "arguments");
          }
        }
      }
    }
  }

  if (is_final) {
    for (auto& [idx, acc] : pending_tools_) {
      backend::ToolCall call;
      call.id = acc.id;
      call.name = acc.name;
      call.arguments = acc.arguments;
      tool_cb(call);
    }
    pending_tools_.clear();
    content_cb(backend::TokenType::kContent, "", true);
  }
}

void OpenAITokenAdapter::Reset() {
  pending_tools_.clear();
}

}  // namespace pu::backends::openai
