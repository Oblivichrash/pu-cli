// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/tools/toolbox.hpp"

#include <string>

#include <boost/json.hpp>

namespace pu::tools {

// How risky a shell command looks to the built-in sandbox heuristics. Lives
// here rather than in the orchestration layer because the assessment is a
// property of the tool that performs it.
enum class RiskLevel { kSafe, kNeutral, kDangerous };

class ExecuteBashToolStandard : public pu::Tool {
 public:
  explicit ExecuteBashToolStandard(std::string sandbox_root);
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;

 private:
  std::string sandbox_root_;
};

class WriteFileTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;
};

class AskUserTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;
};

}  // namespace pu::tools
