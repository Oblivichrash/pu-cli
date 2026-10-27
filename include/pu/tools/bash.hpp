// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <string>

#include <boost/json.hpp>

#include "pu/paging/paging.hpp"
#include "pu/tools/tool.hpp"

namespace pu::tools {

// Runs a shell command and hands back one page of its output.
//
// The store is borrowed, not owned: it belongs to the session, so a rebuild of the
// toolbox — which is what a model switch does — leaves every cursor the model is
// holding still readable. The page size comes from that store for the same reason: a
// tool configured for pages the store cannot address would be a second opinion.
class BashTool : public pu::Tool {
 public:
  BashTool(std::string sandbox_root, paging::Pager& pager);

  std::string Name() const override;
  std::string Description() const override;
  boost::json::value ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;

 private:
  // The lines this page shows: the caller's `limit`, or the configured page size, in
  // every case no more than the configured ceiling.
  std::size_t PageLines(const boost::json::value& args) const;

  // The command and the directory it runs in: what a later run has to share before a
  // page of this one can be called superseded.
  std::string SourceKey(const std::string& command) const;

  std::string Run(const std::string& command, const paging::Anchor& anchor, std::size_t lines,
                  const pu::ToolContext& ctx);
  std::string ReadAt(const paging::Cursor& cursor, std::size_t lines);
  std::string Envelope(const paging::Buffer& buffer, const paging::Page& page) const;

  std::string sandbox_root_;
  paging::Pager& pager_;
};

}  // namespace pu::tools
