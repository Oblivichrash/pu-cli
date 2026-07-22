// SPDX-License-Identifier: GPL-3.0-only
#include "session.hpp"

#include "ui.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace pu::cli {

SessionManager::SessionManager(std::filesystem::path store_dir, agent::AgentManager& manager)
    : store_(store_dir), manager_(manager) {}

bool SessionManager::SaveConversation(const std::string& name, const std::vector<ChatMessage>& messages,
                                     bool no_summary, GlobalContext& global_ctx,
                                     std::shared_ptr<core::Context> root_context) {
  (void)no_summary;   // placeholder for future summary control
  (void)global_ctx;   // reserved for future use
  Conversation conv;
  conv.id = name;
  conv.created_at = messages.empty() ? CurrentTimestamp() : messages.front().timestamp;
  conv.updated_at = CurrentTimestamp();
  conv.messages = messages;
  conv.expert_histories = manager_.SnapshotAgents();

  try {
    store_.Save(conv);
    std::cout << "[INFO] Conversation saved as '" << conv.id << "'\n";
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to save conversation: " << e.what() << "\n";
    return false;
  }

  if (root_context) {
    const char* home = std::getenv("HOME");
    auto pu_dir = std::filesystem::path(home ? home : ".") / ".pu";
    auto context_path = pu_dir / "contexts" / "active" / "root.json";
    root_context->Save(context_path);
  }

  return true;
}

bool SessionManager::LoadConversation(const std::string& id, std::vector<ChatMessage>& messages) {
  try {
    auto conv = store_.Load(id);
    messages = conv.messages;
    manager_.RestoreAgents(conv.expert_histories);
    manager_.SetActiveAgent("");
    std::cout << "[INFO] Loaded conversation '" << id << "'\n";
    return true;
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to load conversation: " << e.what() << "\n";
    return false;
  }
}

std::vector<Conversation> SessionManager::ListConversations() const {
  std::vector<std::string> errors;
  auto convs = store_.List(errors);
  for (const auto& err : errors) {
    std::cout << "  [Warning] " << err << "\n";
  }
  return convs;
}

std::string SessionManager::ExportMarkdown(const std::string& id) const {
  try {
    return store_.ExportMarkdown(id);
  } catch (const std::exception& e) {
    std::cerr << "Error: failed to export conversation: " << e.what() << "\n";
    return "";
  }
}

void SessionManager::AddNote(const std::string& agent_name, const std::string& text, GlobalContext& global_ctx) {
  std::string timestamped_note = "[" + CurrentTimestamp() + "] " + text;
  auto notes_opt = global_ctx.Read("memory/notes/" + agent_name);
  nlohmann::json notes_array = nlohmann::json::array();
  if (notes_opt && notes_opt->is_array()) {
    notes_array = *notes_opt;
  }
  notes_array.push_back(timestamped_note);
  global_ctx.Write("memory/notes/" + agent_name, notes_array);
}

std::vector<std::string> SessionManager::ShowNotes(const std::string& agent_name, GlobalContext& global_ctx) const {
  std::vector<std::string> notes;
  auto notes_opt = global_ctx.Read("memory/notes/" + agent_name);
  if (notes_opt && notes_opt->is_array()) {
    for (const auto& note : *notes_opt) {
      if (note.is_string()) {
        notes.push_back(note.get<std::string>());
      }
    }
  }
  return notes;
}

}  // namespace pu::cli
