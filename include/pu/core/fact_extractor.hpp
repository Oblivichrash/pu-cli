// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/core/context.hpp"
#include "pu/core/fact.hpp"

namespace pu::core {

class FactExtractor {
 public:
  FactExtractor() = default;

  FactList Extract(const std::shared_ptr<Context>& ctx,
                   const std::string& goal);
};

}  // namespace pu::core
