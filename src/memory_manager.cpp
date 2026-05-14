// SPDX-License-Identifier: GPL-3.0-only
#include "pu/memory_manager.hpp"
#include "pu/error_codes.hpp"
#include <fstream>
#include <sstream>

namespace pu {

MemoryManager::MemoryManager(std::filesystem::path cwd)
    : cwd_(std::move(cwd)) {}

std::filesystem::path MemoryManager::MemoryDir() const {
  return cwd_ / ".pu" / "memory";
}

std::filesystem::path MemoryManager::SummaryPath(
    const std::string& expert_name) const {
  return MemoryDir() / (expert_name + ".summary.md");
}

std::filesystem::path MemoryManager::NotesPath(
    const std::string& expert_name) const {
  return MemoryDir() / (expert_name + ".notes.md");
}

std::string MemoryManager::LoadSummary(const std::string& expert_name,
                                       std::error_code& ec) const {
  ec.clear();
  auto path = SummaryPath(expert_name);
  std::ifstream file(path);
  if (!file.is_open()) {
    ec = StoreErrc::not_found;
    return {};
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

void MemoryManager::SaveSummary(const std::string& expert_name,
                                const std::string& content,
                                std::error_code& ec) {
  ec.clear();
  std::filesystem::create_directories(MemoryDir());

  auto path = SummaryPath(expert_name);
  if (std::filesystem::exists(path)) {
    std::filesystem::rename(path, path.string() + ".bak", ec);
  }

  std::ofstream file(path);
  if (!file.is_open()) {
    ec = StoreErrc::write_failed;
    return;
  }
  file << content;
}

void MemoryManager::AppendNote(const std::string& expert_name,
                               const std::string& text,
                               std::error_code& ec) {
  ec.clear();
  std::filesystem::create_directories(MemoryDir());
  std::ofstream file(NotesPath(expert_name), std::ios::app);
  if (!file.is_open()) {
    ec = StoreErrc::write_failed;
    return;
  }
  file << text << "\n";
}

std::string MemoryManager::LoadNotes(const std::string& expert_name,
                                     std::error_code& ec) const {
  ec.clear();
  auto path = NotesPath(expert_name);
  std::ifstream file(path);
  if (!file.is_open()) {
    ec = StoreErrc::not_found;
    return {};
  }
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

}  // namespace pu
