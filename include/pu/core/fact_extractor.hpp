// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/session/workspace.hpp"
#include "pu/session/artifact.hpp"

namespace pu {

class FactExtractor {
 public:
  FactExtractor() = default;

  std::vector<Artifact> Extract(const std::shared_ptr<Workspace>& ctx,
                   const std::string& goal);
};

}  // namespace pu
