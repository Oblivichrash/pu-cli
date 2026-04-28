// SPDX-License-Identifier: GPL-3.0-only

#include "pu/conversation_store.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pu {

using json = nlohmann::json;

ConversationStore::ConversationStore(std::filesystem::path storage_dir)
    : dir_(std::move(storage_dir)) {
  std::filesystem::create_directories(dir_);
}

std::filesystem::path ConversationStore::PathFor(const std::string& id) const {
  return dir_ / (id + ".json");
}

void ConversationStore::Save(const Conversation& conv) {
  json j;
  j["id"] = conv.id;
  j["created_at"] = conv.created_at;
  j["updated_at"] = conv.updated_at;

  json messages = json::array();
  for (const auto& msg : conv.messages) {
    messages.push_back({
      {"id", msg.id},
      {"timestamp", msg.timestamp},
      {"role", msg.role},
      {"content", msg.content}
    });
  }
  j["messages"] = messages;

  json experts = json::object();
  for (const auto& [name, history] : conv.expert_histories) {
    json expert_msgs = json::array();
    for (const auto& msg : history) {
      expert_msgs.push_back({
        {"id", msg.id},
        {"timestamp", msg.timestamp},
        {"role", msg.role},
        {"content", msg.content}
      });
    }
    experts[name] = expert_msgs;
  }
  j["experts"] = experts;

  std::ofstream file(PathFor(conv.id));
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open conversation file for writing: " + conv.id);
  }
  file << j.dump(2);
}

Conversation ConversationStore::Load(const std::string& id) const {
  std::ifstream file(PathFor(id));
  if (!file.is_open()) {
    throw std::runtime_error("Conversation not found: " + id);
  }

  json j;
  try {
    file >> j;
  } catch (const json::parse_error& e) {
    throw std::runtime_error("Invalid conversation file: " + id);
  }

  Conversation conv;
  conv.id = j.value("id", id);
  conv.created_at = j.value("created_at", "");
  conv.updated_at = j.value("updated_at", "");

  for (const auto& item : j["messages"]) {
    ChatMessage msg;
    msg.id = item.value("id", 0);
    msg.timestamp = item.value("timestamp", "");
    msg.role = item.value("role", "");
    msg.content = item.value("content", "");
    conv.messages.push_back(msg);
  }

  if (j.contains("experts")) {
    for (auto& [name, arr] : j["experts"].items()) {
      std::vector<ChatMessage> history;
      for (const auto& item : arr) {
        ChatMessage msg;
        msg.id = item.value("id", 0);
        msg.timestamp = item.value("timestamp", "");
        msg.role = item.value("role", "");
        msg.content = item.value("content", "");
        history.push_back(msg);
      }
      conv.expert_histories[name] = std::move(history);
    }
  }

  return conv;
}

std::vector<Conversation> ConversationStore::List() const {
  std::vector<Conversation> results;
  for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
    if (entry.path().extension() == ".json") {
      auto id = entry.path().stem().string();
      try {
        results.push_back(Load(id));
      } catch (...) {
        // skip invalid files
      }
    }
  }
  return results;
}

std::string ConversationStore::ExportMarkdown(const std::string& id) const {
  auto conv = Load(id);
  std::ostringstream md;
  md << "# Conversation: " << conv.id << "\n\n";
  md << "Created: " << conv.created_at << "\n";
  md << "Updated: " << conv.updated_at << "\n\n";

  for (const auto& msg : conv.messages) {
    md << "**" << msg.role << "** (" << msg.timestamp << "):\n\n";
    md << msg.content << "\n\n---\n\n";
  }
  return md.str();
}

}  // namespace pu
