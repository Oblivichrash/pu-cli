// SPDX-License-Identifier: GPL-3.0-only
//
// Conversation persistence layer.

#pragma once

#include "pu/conversation.hpp"
#include <filesystem>
#include <string>
#include <vector>
#include <system_error>

namespace pu {

class ConversationStore {
 public:
  explicit ConversationStore(std::filesystem::path storage_dir);

  void Save(const Conversation& conv, std::error_code& ec);
  Conversation Load(const std::string& id, std::error_code& ec) const;
  std::vector<Conversation> List() const;
  std::string ExportMarkdown(const std::string& id, std::error_code& ec) const;

 private:
  std::filesystem::path dir_;
  std::filesystem::path PathFor(const std::string& id) const;
};

}  // namespace pu
