// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/conversation.hpp"

#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pu/core/fact.hpp"

namespace pu::core {

using json = nlohmann::json;

class Context {
 public:
  Context() = default;
  explicit Context(std::string id);

  const std::string& GetId() const { return id_; }
  void SetId(const std::string& id) { id_ = id; }

  void Append(const ChatMessage& msg);
  void Append(const std::string& role, const std::string& content);
  std::vector<ChatMessage> GetHistory() const { return history_; }
  std::vector<ChatMessage> Recent(int n) const;
  size_t HistorySize() const { return history_.size(); }
  void ClearHistory();

  void SetVar(const std::string& key, const json& value);
  std::optional<json> GetVar(const std::string& key) const;
  bool HasVar(const std::string& key) const;
  void RemoveVar(const std::string& key);
  std::unordered_map<std::string, json> GetAllVars() const { return vars_; }

  void AddFact(const Fact& fact);
  void AddFacts(const FactList& facts);
  FactList GetFacts() const { return facts_; }
  FactList GetFactsByType(Fact::Type type) const;
  void ClearFacts();

  void Compact();

  void Save(const std::filesystem::path& path) const;
  static std::shared_ptr<Context> Load(const std::filesystem::path& path);
  static std::shared_ptr<Context> LoadOrCreate(const std::filesystem::path& path);

 private:
  std::string id_;
  std::vector<ChatMessage> history_;
  std::unordered_map<std::string, json> vars_;
  FactList facts_;
  size_t max_history_size_ = 1000;
};

}  // namespace pu::core
