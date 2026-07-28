// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "pu/session/session.hpp"

namespace pu {

struct SessionMetadata {
  std::string id;
  std::string owner_id;
  std::string created_at;
  std::string last_access_at;
};

class SessionStore {
public:
  explicit SessionStore(const std::filesystem::path& storage_dir = "");

  void SaveSession(const Session& session);
  std::unique_ptr<Session> LoadSession(const std::string& id) const;
  void DeleteSession(const std::string& id);
  std::vector<SessionMetadata> ListAllMetadata() const;

  // Export a session's conversation history as Markdown
  std::string ExportMarkdown(const std::string& id) const;

private:
  std::filesystem::path dir_;
  std::filesystem::path PathFor(const std::string& id) const;
};

} // namespace pu