// SPDX-License-Identifier: GPL-3.0-only
#include "pu/storage/session_store.hpp"
#include "pu/error.hpp"
#include <spdlog/spdlog.h>
#include <fstream>

namespace pu {

SessionStore::SessionStore(const std::filesystem::path& storage_dir) {
  if (storage_dir.empty()) {
    // Use default path
    auto home = std::getenv("HOME") ? std::getenv("HOME") : ".";
    dir_ = std::filesystem::path(home) / ".pu" / "sessions";
  } else {
    dir_ = storage_dir;
  }
  std::filesystem::create_directories(dir_);
}

std::filesystem::path SessionStore::PathFor(const std::string& id) const {
  return dir_ / (id + ".json");
}

void SessionStore::SaveSession(const Session& session) {
  auto j = session.Serialize();
  std::ofstream file(PathFor(session.GetId()));
  if (!file.is_open()) {
    throw StoreError("Failed to write session file: " + PathFor(session.GetId()).string());
  }
  file << j.dump(2);
}

std::unique_ptr<Session> SessionStore::LoadSession(const std::string& id) const {
  auto path = PathFor(id);
  if (!std::filesystem::exists(path)) {
    return nullptr;
  }
  std::ifstream file(path);
  if (!file.is_open()) {
    return nullptr;
  }
  nlohmann::json j;
  try {
    file >> j;
  } catch (const nlohmann::json::parse_error&) {
    return nullptr;
  }
  return Session::Deserialize(j);
}

void SessionStore::DeleteSession(const std::string& id) {
  auto path = PathFor(id);
  if (std::filesystem::exists(path)) {
    std::filesystem::remove(path);
  }
}

std::vector<SessionMetadata> SessionStore::ListAllMetadata() const {
  std::vector<SessionMetadata> result;
  if (!std::filesystem::exists(dir_)) {
    return result;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
    if (entry.path().extension() == ".json") {
      auto id = entry.path().stem().string();
      std::ifstream file(entry.path());
      if (file.is_open()) {
        try {
          nlohmann::json j;
          file >> j;
          SessionMetadata meta;
          meta.id = j.value("id", id);
          meta.owner_id = j.value("owner_id", "");
          if (j.contains("created_at")) {
            meta.created_at = std::to_string(j["created_at"].get<int64_t>());
          }
          if (j.contains("last_access_at")) {
            meta.last_access_at = std::to_string(j["last_access_at"].get<int64_t>());
          }
          result.push_back(std::move(meta));
        } catch (...) {
          // Skip invalid files
        }
      }
    }
  }
  return result;
}

} // namespace pu