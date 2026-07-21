// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/agent_core.hpp"
#include "pu/conversation_store.hpp"
#include "pu/context.hpp"

namespace pu::cli {

/// Manages session state: conversations, notes, and global context.
class SessionManager {
 public:
  SessionManager(std::filesystem::path store_dir, agent::AgentManager& manager);

  /// Save current panel messages as a conversation
  bool SaveConversation(const std::string& name, const std::vector<ChatMessage>& messages,
                        bool no_summary, GlobalContext& global_ctx);

  /// Load a conversation into panel messages
  bool LoadConversation(const std::string& id, std::vector<ChatMessage>& messages);

  /// List all saved conversations
  std::vector<Conversation> ListConversations() const;

  /// Export a conversation to Markdown string
  std::string ExportMarkdown(const std::string& id) const;

  /// Add a note for the current agent
  void AddNote(const std::string& agent_name, const std::string& text, GlobalContext& global_ctx);

  /// Show notes for the current agent
  std::vector<std::string> ShowNotes(const std::string& agent_name, GlobalContext& global_ctx) const;

 private:
  ConversationStore store_;
  agent::AgentManager& manager_;
};

}  // namespace pu::cli
