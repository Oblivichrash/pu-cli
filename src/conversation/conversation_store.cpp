// SPDX-License-Identifier: GPL-3.0-only
#include "pu/conversation_store.hpp"

#include "pu/error.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <sstream>

namespace pu {

using json = nlohmann::json;

namespace {

json MessageToJson(const ChatMessage& msg) {
  return {{"id", msg.id}, {"timestamp", msg.timestamp}, {"role", msg.role},
          {"content", msg.content}, {"tool_name", msg.tool_name},
          {"tool_calls_json", msg.tool_calls_json}};
}
ChatMessage MessageFromJson(const json& j) {
  return {j.value("id", 0), j.value("timestamp", ""), j.value("role", ""),
          j.value("content", ""), j.value("tool_name", ""),
          j.value("tool_calls_json", "")};
}
json ExpertHistoriesToJson(const auto& histories) {
  json obj = json::object();
  for (const auto& [name, msgs] : histories) {
    json arr = json::array();
    for (const auto& m : msgs) arr.push_back(MessageToJson(m));
    obj[name] = arr;
  }
  return obj;
}
auto ExpertHistoriesFromJson(const json& j) {
  std::unordered_map<std::string, std::vector<ChatMessage>> result;
  for (auto& [name, arr] : j.items()) {
    std::vector<ChatMessage> list;
    for (const auto& item : arr) list.push_back(MessageFromJson(item));
    result[name] = std::move(list);
  }
  return result;
}

}  // namespace

ConversationStore::ConversationStore(std::filesystem::path storage_dir)
    : dir_(std::move(storage_dir)) { std::filesystem::create_directories(dir_); }

std::filesystem::path ConversationStore::PathFor(const std::string& id) const {
  return dir_ / (id + ".json");
}

void ConversationStore::Save(const Conversation& conv) {
  json j;
  j["id"] = conv.id;
  j["created_at"] = conv.created_at;
  j["updated_at"] = conv.updated_at;
  j["messages"] = json::array();
  for (const auto& m : conv.messages) j["messages"].push_back(MessageToJson(m));
  j["experts"] = ExpertHistoriesToJson(conv.expert_histories);
  std::ofstream file(PathFor(conv.id));
  if (!file.is_open()) { throw StoreError("Failed to write conversation file: " + PathFor(conv.id).string()); }
  file << j.dump(2);
}

Conversation ConversationStore::Load(const std::string& id) const {
  std::ifstream file(PathFor(id));
  if (!file.is_open()) { throw StoreError("Conversation not found: " + id); }
  json j;
  try { file >> j; } catch (const json::parse_error&) { throw StoreError("Invalid conversation data for id: " + id); }
  Conversation conv;
  conv.id = j.value("id", id);
  conv.created_at = j.value("created_at", "");
  conv.updated_at = j.value("updated_at", "");
  for (const auto& item : j["messages"]) conv.messages.push_back(MessageFromJson(item));
  if (j.contains("experts")) conv.expert_histories = ExpertHistoriesFromJson(j["experts"]);
  return conv;
}

std::vector<Conversation> ConversationStore::List(std::vector<std::string>& errors) const {
  errors.clear();
  std::vector<Conversation> results;
  for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
    if (entry.path().extension() == ".json") {
      auto id = entry.path().stem().string();
      try {
        auto conv = Load(id);
        results.push_back(std::move(conv));
      } catch (const StoreError& e) {
        errors.push_back(id + ": " + e.what());
      }
    }
  }
  return results;
}

std::vector<Conversation> ConversationStore::List() const {
  std::vector<std::string> ignored;
  return List(ignored);
}

std::string ConversationStore::ExportMarkdown(const std::string& id) const {
  auto conv = Load(id);
  std::ostringstream md;
  md << "# Conversation: " << conv.id << "\n\n"
     << "Created: " << conv.created_at << "\nUpdated: " << conv.updated_at << "\n\n";
  for (const auto& msg : conv.messages) {
    md << "**" << msg.role << "** (" << msg.timestamp << "):\n\n" << msg.content << "\n\n---\n\n";
  }
  return md.str();
}

}  // namespace pu