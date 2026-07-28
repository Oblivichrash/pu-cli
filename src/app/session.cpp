// SPDX-License-Identifier: GPL-3.0-only
#include "session.hpp"

#include "ui.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace pu::cli {

SessionManager::SessionManager(std::filesystem::path store_dir, pu::AgentManager& manager)
    : store_(std::make_shared<pu::SessionStore>(store_dir)), manager_(manager) {}

bool SessionManager::SaveConversation(const std::string& name, const std::vector<ChatMessage>& messages,
                                     bool no_summary,
                                     std::shared_ptr<Workspace> root_context) {
  (void)no_summary;
  
  // Create a session with the given name
  auto session = std::make_unique<pu::Session>(name, "cli");
  
  // Convert ChatMessages to the session's workspace history
  for (const auto& msg : messages) {
    session->GetWorkspace().Append(msg);
  }

  try {
    store_->SaveSession(*session);
    std::cout << "[INFO] Conversation saved as '" << session->GetId() << "'\n";
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to save conversation: " << e.what() << '\n';
    return false;
  }

  if (root_context) {
    auto pu_dir = pu::path::GetDataDir();
    auto context_path = pu_dir / "contexts" / "active" / "root.json";
    root_context->Save(context_path);
  }

  return true;
}

bool SessionManager::LoadConversation(const std::string& id, std::vector<ChatMessage>& messages) {
  try {
    auto session = store_->LoadSession(id);
    if (!session) {
      std::cerr << "Error: session not found: " << id << '\n';
      return false;
    }
    messages.clear();
    auto history = session->GetWorkspace().GetHistory();
    for (const auto& msg : history) {
      ChatMessage cm;
      cm.id = msg.id;
      cm.timestamp = msg.timestamp;
      cm.role = msg.role;
      cm.content = msg.content;
      cm.tool_name = msg.tool_name;
      cm.tool_calls_json = msg.tool_calls_json;
      messages.push_back(std::move(cm));
    }
    std::cout << "[INFO] Loaded conversation '" << id << "'\n";
    return true;
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to load conversation: " << e.what() << '\n';
    return false;
  }
}

std::vector<Conversation> SessionManager::ListConversations() const {
  auto metadata = store_->ListAllMetadata();
  std::vector<Conversation> results;
  for (const auto& meta : metadata) {
    Conversation conv;
    conv.id = meta.id;
    conv.created_at = meta.created_at;
    conv.updated_at = meta.last_access_at;
    results.push_back(std::move(conv));
  }
  return results;
}

std::string SessionManager::ExportMarkdown(const std::string& id) const {
  try {
    auto session = store_->LoadSession(id);
    if (!session) {
      std::cerr << "Error: session not found: " << id << '\n';
      return "";
    }
    std::ostringstream md;
    md << "# Conversation: " << session->GetId() << "\n\n";
    auto history = session->GetWorkspace().GetHistory();
    for (const auto& msg : history) {
      md << "**" << msg.role << "** (" << msg.timestamp << "):\n\n" << msg.content << "\n\n---\n\n";
    }
    return md.str();
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to export conversation: " << e.what() << '\n';
    return "";
  }
}

void SessionManager::AddNote(const std::string& agent_name, const std::string& text,
                             std::shared_ptr<Workspace> root_context) {
  std::string timestamped_note = "[" + CurrentTimestamp() + "] " + text;

  if (root_context) {
    std::string var_key = "notes/" + agent_name;
    auto existing = root_context->GetVar(var_key);
    nlohmann::json new_notes = existing.has_value() ? *existing : nlohmann::json::array();
    if (new_notes.is_array()) {
      new_notes.push_back(timestamped_note);
      root_context->SetVar(var_key, new_notes);
    }
  }
}

std::vector<std::string> SessionManager::ShowNotes(const std::string& agent_name,
                                                    std::shared_ptr<Workspace> root_context) const {
  std::vector<std::string> notes;

  if (!root_context) return notes;

  std::string var_key = "notes/" + agent_name;
  auto val = root_context->GetVar(var_key);
  if (val.has_value() && val->is_array()) {
    for (const auto& item : *val) {
      if (item.is_string()) notes.push_back(item.get<std::string>());
    }
  }
  return notes;
}

}  // namespace pu::cli
