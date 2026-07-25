// SPDX-License-Identifier: GPL-3.0-only
#include "pu/core/context.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pu::core {

namespace {

std::string CurrentTimestamp() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

std::string GenerateForkId() {
  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%d_%H%M%S");
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

  // Fork-Merge metadata
  j["branch_name"] = branch_name_;
  j["state"] = static_cast<int>(state_);
  j["is_merge_commit"] = is_merge_commit_;
  if (merge_message_.has_value()) {
    j["merge_message"] = merge_message_.value();
  }

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

  // Load Fork-Merge metadata
  ctx->branch_name_ = j.value("branch_name", "main");
  ctx->state_ = static_cast<State>(j.value("state", 0));
  ctx->is_merge_commit_ = j.value("is_merge_commit", false);
  if (j.contains("merge_message")) {
    ctx->merge_message_ = j["merge_message"].get<std::string>();
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

// ===== Fork-Merge Implementation =====

std::shared_ptr<Context> Context::Fork(const std::string& branch_name) {
  if (state_ != State::kActive) {
    throw std::runtime_error("Cannot fork: context '" + id_ + "' is not active (state=" +
                             std::to_string(static_cast<int>(state_)) + ")");
  }

  // Generate unique branch name if not provided
  std::string actual_branch = branch_name;
  if (actual_branch.empty()) {
    actual_branch = "fork_" + GenerateForkId();
  }

  // Create child context with deep copy of vars and facts
  auto child = std::make_shared<Context>("ctx-" + actual_branch);
  child->branch_name_ = actual_branch;
  child->parent_ = shared_from_this();
  child->vars_ = vars_;         // Deep copy vars
  child->facts_ = facts_;       // Deep copy facts
  child->max_history_size_ = max_history_size_;

  // Register as child
  children_.push_back(child);

  // Add system message to child indicating fork
  child->Append("system", "Forked from '" + id_ + "' (branch: " + actual_branch + ")");

  return child;
}

std::shared_ptr<Context> Context::Merge(const std::shared_ptr<Context>& child,
                                         const std::string& message) {
  if (!child) {
    throw std::runtime_error("Merge: child context is null");
  }

  if (child->state_ != State::kActive) {
    throw std::runtime_error("Cannot merge: child context '" + child->id_ +
                             "' is not active (state=" + std::to_string(static_cast<int>(child->state_)) + ")");
  }

  // Verify this context is the child's parent
  auto child_parent = child->parent_.lock();
  if (child_parent.get() != this) {
    throw std::runtime_error("Cannot merge: context '" + child->id_ +
                             "' is not a child of '" + id_ + "'");
  }

  // Create a new merge context (like a Git merge commit)
  auto merge_ctx = std::make_shared<Context>("merge-" + child->id_);
  merge_ctx->is_merge_commit_ = true;
  merge_ctx->branch_name_ = branch_name_;
  merge_ctx->merge_message_ = message;
  merge_ctx->vars_ = vars_;        // Inherit parent vars
  merge_ctx->facts_ = facts_;      // Inherit parent facts
  merge_ctx->parent_ = parent_;    // Merge context belongs to parent's lineage
  merge_ctx->max_history_size_ = max_history_size_;

  // Record merge parents
  merge_ctx->merge_parents_.push_back(shared_from_this());
  merge_ctx->merge_parents_.push_back(child);

  // Copy parent history
  for (const auto& msg : history_) {
    merge_ctx->Append(msg);
  }

  // Append merge summary message
  merge_ctx->Append("system", "[Merge] " + message);

  // Copy child's facts into merge context
  merge_ctx->AddFacts(child->GetFacts());

  // Copy child's vars (child vars override parent vars on conflict)
  for (const auto& [key, val] : child->vars_) {
    merge_ctx->vars_[key] = val;
  }

  // Mark child as merged
  child->state_ = State::kMerged;

  // Add merge reference to parent's history
  Append("system", "[Child '" + child->id_ + "' merged] " + message);

  return merge_ctx;
}

std::vector<std::shared_ptr<Context>> Context::GetMergeParents() const {
  std::vector<std::shared_ptr<Context>> result;
  for (const auto& wp : merge_parents_) {
    auto sp = wp.lock();
    if (sp) {
      result.push_back(sp);
    }
  }
  return result;
}

size_t Context::GetTokenCount() const {
  size_t total_chars = 0;
  for (const auto& msg : history_) {
    total_chars += msg.content.size();
  }
  // Rough estimate: ~4 chars per token
  return total_chars / 4;
}

}  // namespace pu::core
