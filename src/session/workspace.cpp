// SPDX-License-Identifier: GPL-3.0-only
#include "pu/session/workspace.hpp"

#include "pu/core/json.hpp"

#include <boost/json.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

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
  msg.timestamp = CurrentTimestamp();
  msg.role = role;
  msg.content = content;
  Append(msg);
}

std::vector<ChatMessage> Workspace::GetHistory() const {
  return transcript_.GetHistory();
}

size_t Workspace::HistorySize() const {
  return transcript_.Size();
}

bool Workspace::HasPendingToolCalls() const {
  return transcript_.HasPendingToolCalls();
}

boost::json::value Workspace::Serialize() const {
  boost::json::value j = boost::json::object{};
  j.as_object()["history"] = transcript_.Serialize();
  return j;
}

std::shared_ptr<Workspace> Workspace::Deserialize(const boost::json::value& j) {
  auto ws = std::make_shared<Workspace>();

  if (json::HasKey(j, "history")) {
    if (!Transcript::Deserialize(j.at("history"), ws->transcript_)) return nullptr;
  }

  return ws;
}

void Workspace::ClearHistory() {
  transcript_ = Transcript{};
}

} // namespace pu
