// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/token_adapter.hpp"
#include "pu/backend.hpp"
#include <map>
#include <string>

namespace pu::backends::openai {

class OpenAITokenAdapter : public ITokenAdapter {
 public:
  void HandleJson(const nlohmann::json& j, backend::ChatCallback content_cb,
                  backend::ToolCallback tool_cb) override;
  void Reset() override;

 private:
  struct ToolCallAccumulator { std::string id, name, arguments; };
  std::map<int, ToolCallAccumulator> pending_tools_;
};

}  // namespace pu::backends::openai
