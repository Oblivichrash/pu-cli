// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <map>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace pu {

class Memory {
public:
  void SetVar(const std::string& key, const nlohmann::json& value);
  std::optional<nlohmann::json> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);

  nlohmann::json Serialize() const;
  static Memory Deserialize(const nlohmann::json& j);

private:
  std::map<std::string, nlohmann::json> variables_;
};

} // namespace pu
