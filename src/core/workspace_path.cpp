// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/workspace_path.hpp"

namespace pu {

std::filesystem::path ResolveWorkspacePath(const std::filesystem::path& root,
                                          const std::string& configured_path) {
  const auto path =
      configured_path.empty() ? std::filesystem::path(".") : std::filesystem::path(configured_path);
  return path.is_absolute() ? path : root / path;
}

}  // namespace pu
