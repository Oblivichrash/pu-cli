// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "pu/tools/tool.hpp"

namespace pu::tools {

// Runs one turn in another directory's `.pu`, and answers with the last thing that turn said.
//
// The point is the boundary, not the call. Whatever that turn reads — files, command output,
// a search that turned up nothing — stays on the other side of it, so this turn's history
// grows by one answer rather than by everything it took to reach one. That is only worth doing
// when the directory can find things out for itself, which is why it runs a whole runtime
// rather than handing a prompt to a model.
//
// It remembers nothing in either direction: the other directory's session is neither read nor
// written, so the same question asked twice costs the same and means the same.
class AskDirTool : public pu::Tool {
 public:
  // `root` is where the asking agent's own `.pu` is, and is what a relative directory names.
  // `depth` is how many askers there already are; at `kMaxDepth` this tool registers nothing,
  // so a directory that asks back cannot go round forever.
  explicit AskDirTool(std::filesystem::path root, std::size_t depth);

  // How many turns deep a delegation may go. Two is enough to ask a specialist that asks an
  // index, and small enough that the cost of a mistake is bounded.
  static constexpr std::size_t kMaxDepth = 2;

  std::string Name() const override;
  std::string Description() const override;
  boost::json::value ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;

 private:
  std::filesystem::path root_;
  std::size_t depth_ = 0;
};

}  // namespace pu::tools
