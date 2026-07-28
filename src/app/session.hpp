// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/agent/agent_manager.hpp"
#include "pu/storage/session_store.hpp"
#include "pu/session/workspace.hpp"
#include "pu/path_utils.hpp"
#include "pu/conversation.hpp"

namespace pu::cli {

/// Manages session state: conversations, notes, and global context.
class SessionManager {
 public:
  SessionManager(std::filesystem::path store_dir, pu::AgentManager& manager);

  bool SaveConversation(const std::string& name, const std::vector<ChatMessage>& messages,
                        bool no_summary,
                        std::shared_ptr<Workspace> root_context = nullptr);

  bool LoadConversation(const std::string& id, std::vector<ChatMessage>& messages);

  std::vector<Conversation> ListConversations() const;

  std::string ExportMarkdown(const std::string& id) const;

  void AddNote(const std::string& agent_name, const std::string& text,
               std::shared_ptr<Workspace> root_context);

  std::vector<std::string> ShowNotes(const std::string& agent_name,
                                     std::shared_ptr<Workspace> root_context) const;

 private:
  std::shared_ptr<pu::SessionStore> store_;
  pu::AgentManager& manager_;
};

}  // namespace pu::cli