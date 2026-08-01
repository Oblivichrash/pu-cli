// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/tools/tool.hpp"

#include <memory>

namespace pu::tools {

// The tool Name() strings ("fork_context"/"merge_context") are the stable
// model-facing identifiers (they appear in agent tool lists and saved
// transcripts). They are intentionally NOT renamed to "workspace" to avoid
// breaking the agent interface; only the C++ class names use Workspace.
class ForkWorkspaceTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, pu::ToolContext& ctx) override;
};

class MergeWorkspaceTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, pu::ToolContext& ctx) override;
};

class ListForksTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  std::string ParametersSchema() const override;
  std::string Execute(const nlohmann::json& args, pu::ToolContext& ctx) override;
};

}  // namespace pu::tools
