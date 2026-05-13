// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace pu {

class MemoryManager {
 public:
  explicit MemoryManager(std::filesystem::path cwd);

  std::string LoadSummary(const std::string& expert_name,
                          std::error_code& ec) const;
  void SaveSummary(const std::string& expert_name,
                   const std::string& content,
                   std::error_code& ec);

  void AppendNote(const std::string& expert_name,
                  const std::string& text,
                  std::error_code& ec);
  std::string LoadNotes(const std::string& expert_name,
                        std::error_code& ec) const;

 private:
  std::filesystem::path MemoryDir() const;
  std::filesystem::path SummaryPath(const std::string& expert_name) const;
  std::filesystem::path NotesPath(const std::string& expert_name) const;

  std::filesystem::path cwd_;
};

}  // namespace pu
