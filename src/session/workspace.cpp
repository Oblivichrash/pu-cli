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

} // namespace

Workspace::Workspace(const std::string& id)
  : id_(id),
      transcript_(std::make_unique<Transcript>()),
      memory_(std::make_unique<Memory>()) {}

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

void Workspace::Compact(size_t keep_head, size_t keep_tail) {
  if (transcript_) transcript_->Compact(keep_head, keep_tail);
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
  if (memory_) memory_->RemoveVar(key);
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
    j["artifacts"] = mem_j["artifacts"];
  } else {
    j["variables"] = nlohmann::json::object();
    j["artifacts"] = nlohmann::json::array();
  }

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
  // "artifacts" is the current key; "facts" remains as a legacy fallback.
  mem_j["artifacts"] = j.contains("artifacts")
      ? j["artifacts"] : j.value("facts", nlohmann::json::array());
  *ws->memory_ = Memory::Deserialize(mem_j);

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
