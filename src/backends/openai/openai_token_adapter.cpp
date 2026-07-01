// SPDX-License-Identifier: GPL-3.0-only
#include "backends/openai/openai_token_adapter.hpp"
#include <nlohmann/json.hpp>

namespace pu::backends::openai {

using json = nlohmann::json;

static std::string SafeString(const json& j, const char* key) {
  return (j.contains(key) && j[key].is_string()) ? j[key].get<std::string>() : "";
}

void OpenAITokenAdapter::HandleJson(const json& j,
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

  if (is_final) {
    for (auto& [idx, acc] : pending_tools_) {
      tool_cb({acc.id, acc.name, acc.arguments});
    }
    pending_tools_.clear();
    content_cb(backend::TokenType::kContent, "", true);
  }
}

void OpenAITokenAdapter::Reset() { pending_tools_.clear(); }

}  // namespace pu::backends::openai
