// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/json.hpp>

#include "pu/llm/llm_provider.hpp"
#include "pu/tools/tool.hpp"

namespace pu {

class Toolbox {
 public:
  void Clear();
  bool RegisterTool(std::unique_ptr<Tool> tool);
  std::vector<std::string> Names() const;
  std::vector<ToolDefinition> GetToolDefinitions() const;
  std::string ExecuteTool(const std::string& name, const boost::json::value& args,
                          ToolContext& ctx);

 private:
  static std::string SanitizeToolName(const std::string& name);

  std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace pu
