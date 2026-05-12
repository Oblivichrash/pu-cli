// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/token_adapter.hpp"

namespace pu::backends::ollama {

class OllamaTokenAdapter : public ITokenAdapter {
 public:
  void HandleJson(const nlohmann::json& j,
                  backend::ChatCallback content_cb,
                  backend::ToolCallback tool_cb) override;
};

}  // namespace pu::backends::ollama
