// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

namespace pu::config {

struct SecurityPolicy {
  std::string sandbox_root;
  size_t max_command_length = 0;
  std::vector<std::string> forbidden_patterns;
};

}  // namespace pu::config
