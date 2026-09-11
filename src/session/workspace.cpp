// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/workspace.hpp"

#include "pu/core/json.hpp"

#include <boost/json.hpp>
#include <chrono>
#include <ctime>
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

void Workspace::Append(const ChatMessage& msg) {
  transcript_.Append(msg);
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
  return transcript_.GetHistory();
}

std::vector<ChatMessage> Workspace::Recent(int n) const {
  return transcript_.Recent(n);
}

size_t Workspace::HistorySize() const {
  return transcript_.Size();
}

void Workspace::Compact(size_t keep_head, size_t keep_tail) {
  transcript_.Compact(keep_head, keep_tail);
}

bool Workspace::HasPendingToolCalls() const {
  return transcript_.HasPendingToolCalls();
}

void Workspace::SetVar(const std::string& key, const boost::json::value& value) {
  memory_.SetVar(key, value);
}

std::optional<boost::json::value> Workspace::GetVar(const std::string& key) const {
  return memory_.GetVar(key);
}

bool Workspace::HasVar(const std::string& key) const {
  return memory_.HasVar(key);
}

void Workspace::RemoveVar(const std::string& key) {
  memory_.RemoveVar(key);
}

void Workspace::AddArtifact(const Artifact& artifact) {
  memory_.AddArtifact(artifact);
}

std::vector<Artifact> Workspace::GetArtifacts() const {
  return memory_.GetArtifacts();
}

void Workspace::ClearArtifacts() {
  memory_.ClearArtifacts();
}

boost::json::value Workspace::Serialize() const {
  boost::json::value j = boost::json::object{};

  j.as_object()["history"] = transcript_.Serialize();
  auto mem_j = memory_.Serialize();
  j.as_object()["variables"] = mem_j.at("variables");
  j.as_object()["artifacts"] = mem_j.at("artifacts");

  return j;
}

std::shared_ptr<Workspace> Workspace::Deserialize(const boost::json::value& j) {
  auto ws = std::make_shared<Workspace>();

  if (json::HasKey(j, "history") && j.at("history").is_array()) {
    ws->transcript_ = Transcript::Deserialize(j.at("history"));
  }

  boost::json::value mem_j = boost::json::object{};
  mem_j.as_object()["variables"] =
      json::ValueOrDefault<boost::json::value>(j, "variables", boost::json::object{});
    mem_j.as_object()["artifacts"] =
      json::ValueOrDefault<boost::json::value>(j, "artifacts", boost::json::array{});
  ws->memory_ = Memory::Deserialize(mem_j);

  return ws;
}

void Workspace::ClearHistory() {
  transcript_ = Transcript{};
}

} // namespace pu
