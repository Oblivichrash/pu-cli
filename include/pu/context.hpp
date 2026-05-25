// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace pu {

using json = nlohmann::json;

class GlobalContext {
 public:
  static std::shared_ptr<GlobalContext> Create(const std::filesystem::path& base_dir = {});

  std::optional<json> Read(const std::string& path) const;
  void Write(const std::string& path, const json& value);

  void Load();
  void Save() const;

 private:
  GlobalContext() = default;
  void ImportLegacyMemory(const std::filesystem::path& memory_dir);
  void EnsureLoaded();

  mutable json root_;
  std::filesystem::path context_path_;
  bool loaded_ = false;
};

}  // namespace pu
