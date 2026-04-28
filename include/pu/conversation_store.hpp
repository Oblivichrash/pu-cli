// SPDX-License-Identifier: GPL-3.0-only
//
// Conversation persistence layer.

#pragma once

#include "pu/conversation.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace pu {

class ConversationStore {
 public:
  explicit ConversationStore(std::filesystem::path storage_dir);

  void Save(const Conversation& conv);
  Conversation Load(const std::string& id) const;
  std::vector<Conversation> List() const;
  std::string ExportMarkdown(const std::string& id) const;

 private:
  std::filesystem::path dir_;
  std::filesystem::path PathFor(const std::string& id) const;
};

}  // namespace pu
