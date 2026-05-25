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
  static std::shared_ptr<GlobalContext> Create();

  std::optional<json> Read(const std::string& path) const;
  void Write(const std::string& path, const json& value);

  void LoadFromDisk(const std::filesystem::path& data_dir);
  void SaveToDisk(const std::filesystem::path& data_dir) const;

 private:
  GlobalContext() = default;

  json root_;
};

}  // namespace pu
