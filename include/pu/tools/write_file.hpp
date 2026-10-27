// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

#include <boost/json.hpp>

#include "pu/tools/tool.hpp"

namespace pu::tools {

// Writes text to a file inside the sandbox. The path is resolved and compared by
// component, so a sandbox of `/tmp/safe` does not admit `/tmp/safe-evil`.
class WriteFileTool : public pu::Tool {
 public:
  std::string Name() const override;
  std::string Description() const override;
  boost::json::value ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;
};

}  // namespace pu::tools
