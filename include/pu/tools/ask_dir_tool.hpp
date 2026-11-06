// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "pu/tools/tool.hpp"

namespace pu::tools {

// Asks a question of an agent, and answers with the last thing that agent said.
//
// The point is the boundary, not the call. Whatever the turn reads — files, command output, a
// search that turned up nothing — stays on the other side of it, so this conversation grows by
// one answer rather than by everything it took to reach one. That is only worth doing when the
// agent can find things out for itself, which is why it runs a whole runtime rather than
// handing a prompt to a model.
//
// Where that agent runs follows from what was named: a directory is a world of its own (its own
// table, its own sandbox), while a named agent with no directory is another entry in this one.
class AskDirTool : public pu::Tool {
 public:
  // `root` is where the asking agent's own `.pu` is, and is what a relative directory names.
  // `allowed` is the agents this one was told it may ask; a directory names its own agent, so
  // the list bounds what a directory can reach as well as what a name can.
  AskDirTool(std::filesystem::path root, std::vector<std::string> allowed);

  std::string Name() const override;
  std::string Description() const override;
  boost::json::value ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;

 private:
  std::filesystem::path root_;
  std::vector<std::string> allowed_;
};

}  // namespace pu::tools
