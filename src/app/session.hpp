// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/agent_core.hpp"
#include "pu/conversation_store.hpp"
#include "pu/context.hpp"
#include "pu/core/context.hpp"

namespace pu::cli {

/// Manages session state: conversations, notes, and global context.
class SessionManager {
 public:
  SessionManager(std::filesystem::path store_dir, agent::AgentManager& manager);

  bool SaveConversation(const std::string& name, const std::vector<ChatMessage>& messages,
                        bool no_summary, GlobalContext& global_ctx,
                        std::shared_ptr<core::Context> root_context = nullptr);

  bool LoadConversation(const std::string& id, std::vector<ChatMessage>& messages);

  std::vector<Conversation> ListConversations() const;

  std::string ExportMarkdown(const std::string& id) const;

  void AddNote(const std::string& agent_name, const std::string& text, GlobalContext& global_ctx);

  std::vector<std::string> ShowNotes(const std::string& agent_name, GlobalContext& global_ctx) const;

 private:
  ConversationStore store_;
  agent::AgentManager& manager_;
};

}  // namespace pu::cli
