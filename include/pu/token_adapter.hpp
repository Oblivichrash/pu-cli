// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/backend.hpp"
#include <nlohmann/json_fwd.hpp>

namespace pu::backends {

class ITokenAdapter {
 public:
  virtual ~ITokenAdapter() = default;

  virtual void HandleJson(const nlohmann::json& j,
                          backend::ChatCallback content_cb,
                          backend::ToolCallback tool_cb) = 0;

  virtual void Reset() {}
};

}  // namespace pu::backends
