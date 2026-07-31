// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/workspace.hpp"
#include "pu/error.hpp"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace pu {

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

} // namespace

Workspace::Workspace(const std::string& id)
  : id_(id),
      transcript_(std::make_unique<Transcript>()),
      memory_(std::make_unique<Memory>()),
      graph_(std::make_unique<RevisionGraph>()) {}

// Transcript delegation
void Workspace::Append(const ChatMessage& msg) {
  if (!transcript_) transcript_ = std::make_unique<Transcript>();
  transcript_->Append(msg);
}

void Workspace::Append(const std::string& role, const std::string& content) {
  ChatMessage msg;
  msg.id = static_cast<int>(HistorySize()) + 1;
  msg.timestamp = CurrentTimestamp();
  msg.role = role;
  msg.content = content;
  Append(msg);
}

std::vector<ChatMessage> Workspace::GetHistory() const {
  if (!transcript_) return {};
  return transcript_->GetHistory();
}

std::vector<ChatMessage> Workspace::Recent(int n) const {
  if (!transcript_) return {};
  return transcript_->Recent(n);
}

size_t Workspace::HistorySize() const {
  if (!transcript_) return 0;
  return transcript_->Size();
}

void Workspace::Compact() {
  if (transcript_) transcript_->Compact();
}

bool Workspace::HasPendingToolCalls() const {
  if (!transcript_) return false;
  return transcript_->HasPendingToolCalls();
}

// Memory delegation
void Workspace::SetVar(const std::string& key, const nlohmann::json& value) {
  if (!memory_) memory_ = std::make_unique<Memory>();
  memory_->SetVar(key, value);
}

std::optional<nlohmann::json> Workspace::GetVar(const std::string& key) const {
  if (!memory_) return std::nullopt;
  return memory_->GetVar(key);
}

bool Workspace::HasVar(const std::string& key) const {
  if (!memory_) return false;
  return memory_->HasVar(key);
}

void Workspace::RemoveVar(const std::string& key) {
  if (memory_) {
    // We don't have a RemoveVar on Memory directly, but we can set it to null
    memory_->SetVar(key, nlohmann::json());
  }
}

void Workspace::AddArtifact(const Artifact& artifact) {
  if (!memory_) memory_ = std::make_unique<Memory>();
  memory_->AddArtifact(artifact);
}

std::vector<Artifact> Workspace::GetArtifacts() const {
  if (!memory_) return {};
  return memory_->GetArtifacts();
}

void Workspace::ClearArtifacts() {
  if (memory_) memory_->ClearArtifacts();
}

// Branch operations
std::shared_ptr<Workspace> Workspace::Fork(const std::string& branch_name) {
  if (state_ != State::kActive) {
    throw pu::Error("Cannot fork: workspace '" + id_ + "' is not active (state=" +
            std::to_string(static_cast<int>(state_)) + ")");
  }

  std::string actual_branch = branch_name;
  if (actual_branch.empty()) {
    actual_branch = "fork_" + GenerateForkId();
  }

  auto child = std::make_shared<Workspace>("ctx-" + actual_branch);
  child->branch_name_ = actual_branch;
  child->parent_ = shared_from_this();

  // Copy memory
  if (memory_) {
    child->memory_ = std::make_unique<Memory>();
    // Copy vars and artifacts
    auto vars_j = memory_->Serialize();
    *child->memory_ = Memory::Deserialize(vars_j);
  }

  children_.push_back(child);

  child->Append("system", "Forked from '" + id_ + "' (branch: " + actual_branch + ")");

  return child;
}

std::shared_ptr<Workspace> Workspace::Merge(const std::shared_ptr<Workspace>& child,
                      const std::string& message) {
  if (!child) {
    throw pu::Error("Merge: child workspace is null");
  }

  if (child->state_ != State::kActive) {
    throw pu::Error("Cannot merge: child workspace '" + child->id_ +
            "' is not active (state=" + std::to_string(static_cast<int>(child->state_)) + ")");
  }

  auto child_parent = child->parent_.lock();
  if (child_parent.get() != this) {
    throw pu::Error("Cannot merge: workspace '" + child->id_ +
            "' is not a child of '" + id_ + "'");
  }

  auto merge_ws = std::make_shared<Workspace>("merge-" + child->id_);
  merge_ws->is_merge_commit_ = true;
  merge_ws->branch_name_ = branch_name_;
  merge_ws->parent_ = parent_;
  merge_ws->transcript_ = std::make_unique<Transcript>();
  merge_ws->memory_ = std::make_unique<Memory>();

  // Copy parent's memory
  if (memory_) {
    auto vars_j = memory_->Serialize();
    *merge_ws->memory_ = Memory::Deserialize(vars_j);
  }

  // Record merge parents
  merge_ws->merge_parents_.push_back(shared_from_this());
  merge_ws->merge_parents_.push_back(child);

  // Copy parent history
  if (transcript_) {
    for (const auto& msg : transcript_->GetHistory()) {
      merge_ws->Append(msg);
    }
  }

  merge_ws->Append("system", "[Merge] " + message);

  // Copy child's artifacts
  if (child->memory_) {
    for (const auto& a : child->memory_->GetArtifacts()) {
      merge_ws->AddArtifact(a);
    }
    // Copy child vars
    auto child_vars_j = child->memory_->Serialize();
    if (child_vars_j.contains("variables") && child_vars_j["variables"].is_object()) {
      for (auto& [key, val] : child_vars_j["variables"].items()) {
        merge_ws->memory_->SetVar(key, val);
      }
    }
  }

  child->state_ = State::kMerged;

  Append("system", "[Child '" + child->id_ + "' merged] " + message);

  return merge_ws;
}

std::vector<std::shared_ptr<Workspace>> Workspace::GetMergeParents() const {
  std::vector<std::shared_ptr<Workspace>> result;
  for (const auto& wp : merge_parents_) {
    auto sp = wp.lock();
    if (sp) {
      result.push_back(sp);
    }
  }
  return result;
}

size_t Workspace::RemoveMergedChildren() {
  size_t removed = 0;
  auto it = children_.begin();
  while (it != children_.end()) {
    if ((*it)->state_ == State::kMerged) {
      it = children_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

size_t Workspace::GetTokenCount() const {
  size_t total_chars = 0;
  if (transcript_) {
    for (const auto& msg : transcript_->GetHistory()) {
      total_chars += msg.content.size();
    }
  }
  return total_chars / 4;
}

// Serialization - compatible with old Context JSON structure
nlohmann::json Workspace::Serialize() const {
  nlohmann::json j;
  j["id"] = id_;

  if (transcript_) {
    j["history"] = transcript_->Serialize();
  } else {
    j["history"] = nlohmann::json::array();
  }

  if (memory_) {
    auto mem_j = memory_->Serialize();
    j["variables"] = mem_j["variables"];
    j["facts"] = mem_j["facts"];
  } else {
    j["variables"] = nlohmann::json::object();
    j["facts"] = nlohmann::json::array();
  }

  j["branch_name"] = branch_name_;
  j["state"] = static_cast<int>(state_);
  j["is_merge_commit"] = is_merge_commit_;

  return j;
}

void Workspace::Save(const std::filesystem::path& path) const {
  auto j = Serialize();
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path);
  if (file.is_open()) {
    file << j.dump(2);
  }
}

std::shared_ptr<Workspace> Workspace::Load(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return nullptr;
  }

  nlohmann::json j;
  try {
    file >> j;
  } catch (const std::exception&) {
    return nullptr;
  }

  return Deserialize(j);
}

std::shared_ptr<Workspace> Workspace::LoadOrCreate(const std::filesystem::path& path) {
  auto ws = Load(path);
  if (!ws) {
    ws = std::make_shared<Workspace>(path.stem().string());
  }
  return ws;
}

std::shared_ptr<Workspace> Workspace::Deserialize(const nlohmann::json& j) {
  auto ws = std::make_shared<Workspace>();
  ws->id_ = j.value("id", "");
  ws->transcript_ = std::make_unique<Transcript>();

  if (j.contains("history") && j["history"].is_array()) {
    *ws->transcript_ = Transcript::Deserialize(j["history"]);
  }

  ws->memory_ = std::make_unique<Memory>();
  nlohmann::json mem_j;
  mem_j["variables"] = j.value("variables", nlohmann::json::object());
  mem_j["facts"] = j.value("facts", nlohmann::json::array());
  *ws->memory_ = Memory::Deserialize(mem_j);

  ws->branch_name_ = j.value("branch_name", "main");
  ws->state_ = static_cast<State>(j.value("state", 0));
  ws->is_merge_commit_ = j.value("is_merge_commit", false);

  return ws;
}

void Workspace::ClearHistory() {
  if (!transcript_) {
    transcript_ = std::make_unique<Transcript>();
  } else {
    transcript_ = std::make_unique<Transcript>(); // replace with empty
  }
}

} // namespace pu
