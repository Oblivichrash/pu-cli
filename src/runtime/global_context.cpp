// SPDX-License-Identifier: GPL-3.0-only
#include "pu/global_context.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace pu {

std::shared_ptr<GlobalContext> GlobalContext::Create(const std::filesystem::path& base_dir) {
  auto ctx = std::shared_ptr<GlobalContext>(new GlobalContext());
  if (!base_dir.empty()) {
    ctx->context_path_ = base_dir / "context.json";
  }
  return ctx;
}

void GlobalContext::EnsureLoaded() {
  if (loaded_) return;
  loaded_ = true;

  if (context_path_.empty()) {
    return;
  }

  if (std::filesystem::exists(context_path_)) {
    std::ifstream file(context_path_);
    if (file.is_open()) {
      try {
        file >> root_;
      } catch (const std::exception& e) {
        std::cerr << "[GlobalContext] Failed to parse " << context_path_ << ": " << e.what() << '\n';
      }
    }
  } else {
    std::filesystem::path memory_dir = context_path_.parent_path() / "memory";
    if (std::filesystem::exists(memory_dir)) {
      ImportLegacyMemory(memory_dir);
    }
  }
}

void GlobalContext::ImportLegacyMemory(const std::filesystem::path& memory_dir) {
  for (const auto& entry : std::filesystem::directory_iterator(memory_dir)) {
    std::string filename = entry.path().filename().string();
    if (filename.find(".summary.md") != std::string::npos) {
      std::string expert_name = filename.substr(0, filename.find(".summary.md"));
      std::ifstream file(entry.path());
      if (file.is_open()) {
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        root_["memory"]["summaries"][expert_name]["latest"] = content;
      }
    } else if (filename.find(".notes.md") != std::string::npos) {
      std::string expert_name = filename.substr(0, filename.find(".notes.md"));
      std::ifstream file(entry.path());
      if (file.is_open()) {
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        json notes_array = json::array();
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
          if (!line.empty()) {
            notes_array.push_back(line);
          }
        }
        root_["memory"]["notes"][expert_name] = notes_array;
      }
    }
  }
  Save();
}

void GlobalContext::Load() {
  EnsureLoaded();
}

void GlobalContext::Save() const {
  if (context_path_.empty()) {
    return;
  }
  std::filesystem::create_directories(context_path_.parent_path());
  std::ofstream file(context_path_);
  if (file.is_open()) {
    file << root_.dump(2);
  } else {
    std::cerr << "[GlobalContext] Failed to save to " << context_path_ << '\n';
  }
}

std::optional<json> GlobalContext::Read(const std::string& path) const {
  const_cast<GlobalContext*>(this)->EnsureLoaded();
  json* current = &root_;
  size_t start = 0;
  size_t end = path.find('/');
  while (end != std::string::npos) {
    std::string key = path.substr(start, end - start);
    if (!current->is_object() || !current->contains(key)) {
      return std::nullopt;
    }
    current = &(*current)[key];
    start = end + 1;
    end = path.find('/', start);
  }
  std::string last_key = path.substr(start);
  if (!current->is_object() || !current->contains(last_key)) {
    return std::nullopt;
  }
  return (*current)[last_key];
}

void GlobalContext::Write(const std::string& path, const json& value) {
  EnsureLoaded();
  json* current = &root_;
  size_t start = 0;
  size_t end = path.find('/');
  while (end != std::string::npos) {
    std::string key = path.substr(start, end - start);
    if (!current->is_object()) {
      *current = json::object();
    }
    if (!current->contains(key)) {
      (*current)[key] = json::object();
    }
    current = &(*current)[key];
    start = end + 1;
    end = path.find('/', start);
  }
  std::string last_key = path.substr(start);
  (*current)[last_key] = value;
  Save();
}

}  // namespace pu
