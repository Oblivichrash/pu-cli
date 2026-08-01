// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace pu::path {

// Get data directory, priority:
// 1. PU_HOME environment variable
// 2. Project-level .pu/ directory (current working directory, if exists)
// 3. User-level ~/.pu/
inline std::filesystem::path GetDataDir() {
  // 1. Environment variable first
  if (const char* env = std::getenv("PU_HOME")) {
    return std::filesystem::path(env);
  }

  // 2. Project-level .pu/ (current working directory)
  std::filesystem::path project_pu = std::filesystem::current_path() / ".pu";
  if (std::filesystem::exists(project_pu)) {
    return project_pu;
  }

  // 3. User-level ~/.pu/
  const char* home = std::getenv("HOME");
  if (home) {
    return std::filesystem::path(home) / ".pu";
  }

  // 4. Fallback
  return std::filesystem::current_path() / ".pu";
}

// Helper: ensure data directory exists

}  // namespace pu::path