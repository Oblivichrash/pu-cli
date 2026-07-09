// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "core/tool.hpp"
#include <string>

namespace pu::tools {

class PythonTool : public agent::Tool {
 public:
  explicit PythonTool(const std::string& file_path);
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, agent::ToolContext& ctx) override;

 private:
  void Parse(const std::string& content);
  std::string ExecutePython(const std::string& args_json) const;

  std::string name_;
  std::string description_;
  std::string parameters_schema_;
  std::string python_code_;
  std::string file_path_;
  int timeout_seconds_ = 5;
  size_t output_limit_ = 4096;
};

}  // namespace pu::tools
