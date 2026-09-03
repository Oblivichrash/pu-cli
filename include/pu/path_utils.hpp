// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace pu::path {

// Get data directory, priority:
// 1. PU_HOME environment variable
// 2. Project-level .pu/ directory (current working directory)
inline std::filesystem::path GetDataDir() {
  // 1. Environment variable first
  if (const char* env = std::getenv("PU_HOME")) {
    return std::filesystem::path(env);
  }

  // 2. Project-level .pu/ (current working directory)
  return std::filesystem::current_path() / ".pu";
}

}  // namespace pu::path
