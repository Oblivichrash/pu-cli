// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/context.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace pu::core {

namespace {

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

}  // namespace

Context::Context(std::string id) : id_(std::move(id)) {}

void Context::Append(const ChatMessage& msg) {
  history_.push_back(msg);
  if (history_.size() > max_history_size_) {
    Compact();
  }
}

void Context::Append(const std::string& role, const std::string& content) {
  ChatMessage msg;
  msg.id = static_cast<int>(history_.size()) + 1;
  msg.timestamp = CurrentTimestamp();
  msg.role = role;
  msg.content = content;
  Append(msg);
}

std::vector<ChatMessage> Context::Recent(int n) const {
  if (n <= 0) return {};
  if (static_cast<size_t>(n) >= history_.size()) return history_;
  return std::vector<ChatMessage>(history_.end() - n, history_.end());
}

void Context::ClearHistory() {
  history_.clear();
}

void Context::SetVar(const std::string& key, const json& value) {
  vars_[key] = value;
}

std::optional<json> Context::GetVar(const std::string& key) const {
  auto it = vars_.find(key);
  if (it == vars_.end()) return std::nullopt;
  return std::optional<json>(it->second);
}

bool Context::HasVar(const std::string& key) const {
  return vars_.find(key) != vars_.end();
}

void Context::RemoveVar(const std::string& key) {
  vars_.erase(key);
}

void Context::AddFact(const Fact& fact) {
  facts_.push_back(fact);
}

void Context::AddFacts(const FactList& facts) {
  facts_.insert(facts_.end(), facts.begin(), facts.end());
}

FactList Context::GetFactsByType(Fact::Type type) const {
  FactList result;
  for (const auto& f : facts_) {
    if (f.type == type) result.push_back(f);
  }
  return result;
}

void Context::ClearFacts() {
  facts_.clear();
}

void Context::Compact() {
  if (history_.size() <= max_history_size_) return;
  const size_t keep_first = 10;
  const size_t keep_last = 50;
  if (history_.size() <= keep_first + keep_last) return;

  std::vector<ChatMessage> compressed;
  compressed.reserve(keep_first + 1 + keep_last);

  compressed.insert(compressed.end(), history_.begin(), history_.begin() + keep_first);

  ChatMessage summary;
  summary.id = static_cast<int>(compressed.size()) + 1;
  summary.timestamp = CurrentTimestamp();
  summary.role = "system";
  summary.content = "[Compressed: " + std::to_string(history_.size() - keep_first - keep_last) + " messages omitted]";
  compressed.push_back(summary);

  compressed.insert(compressed.end(), history_.end() - keep_last, history_.end());

  history_ = std::move(compressed);
}

void Context::Save(const std::filesystem::path& path) const {
  json j;
  j["id"] = id_;

  json history_arr = json::array();
  for (const auto& msg : history_) {
    history_arr.push_back({
      {"id", msg.id},
      {"timestamp", msg.timestamp},
      {"role", msg.role},
      {"content", msg.content},
      {"tool_name", msg.tool_name}
    });
  }
  j["history"] = history_arr;

  json vars_obj = json::object();
  for (const auto& [key, val] : vars_) {
    vars_obj[key] = val;
  }
  j["variables"] = vars_obj;

  json facts_arr = json::array();
  for (const auto& f : facts_) {
    facts_arr.push_back({
      {"type", static_cast<int>(f.type)},
      {"content", f.content},
      {"source", f.source},
      {"confidence", f.confidence}
    });
  }
  j["facts"] = facts_arr;

  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  if (file.is_open()) {
    file << j.dump(2);
  }
}

std::shared_ptr<Context> Context::Load(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return nullptr;
  }

  json j;
  try {
    file >> j;
  } catch (const std::exception&) {
    return nullptr;
  }

  auto ctx = std::make_shared<Context>();
  ctx->id_ = j.value("id", path.stem().string());

  if (j.contains("history") && j["history"].is_array()) {
    for (const auto& item : j["history"]) {
      ChatMessage msg;
      msg.id = item.value("id", 0);
      msg.timestamp = item.value("timestamp", "");
      msg.role = item.value("role", "");
      msg.content = item.value("content", "");
      msg.tool_name = item.value("tool_name", "");
      ctx->history_.push_back(msg);
    }
  }

  if (j.contains("variables") && j["variables"].is_object()) {
    for (auto& [key, val] : j["variables"].items()) {
      ctx->vars_[key] = val;
    }
  }

  if (j.contains("facts") && j["facts"].is_array()) {
    for (const auto& item : j["facts"]) {
      Fact f;
      f.type = static_cast<Fact::Type>(item.value("type", 0));
      f.content = item.value("content", "");
      f.source = item.value("source", "");
      f.confidence = item.value("confidence", 1.0);
      ctx->facts_.push_back(f);
    }
  }

  return ctx;
}

std::shared_ptr<Context> Context::LoadOrCreate(const std::filesystem::path& path) {
  auto ctx = Load(path);
  if (!ctx) {
    ctx = std::make_shared<Context>(path.stem().string());
  }
  return ctx;
}

}  // namespace pu::core
