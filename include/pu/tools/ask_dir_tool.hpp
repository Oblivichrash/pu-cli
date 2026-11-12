// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <filesystem>
#include <string>

#include <boost/json.hpp>

#include "pu/core/base.hpp"
#include "pu/paging/paging.hpp"
#include "pu/runtime/delegation.hpp"
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
// table, its own sandbox), while a named agent is another entry in this one. Both are the same
// question — who answers — so the call asks it once, and `allowed` decides which of the two a
// name was.
class AskDirTool : public pu::Tool {
 public:
  // `root` is where the asking agent's own `.pu` is, and is what a relative directory names.
  // `allowed` is the agents this one was told it may ask; a directory names its own agent, so
  // the list bounds what a directory can reach as well as what a name can.
  // `cancel_token` is the asking turn's stop, handed down rather than copied: the point is that
  // a stop reaches the turn that is actually waiting, which is the delegated one.
  //
  // `pager` is where an answer is recorded: not so it can be paged -- an answer has no window --
  // but so it takes a rank and a source, which is what lets a second answer to the same
  // question elide the first. It is the asking runtime's own store, so its ranks continue the
  // transcript's. Null means the answer is returned unrecorded, and the cost is that asking one
  // question twice keeps both answers.
  AskDirTool(std::filesystem::path root, std::vector<std::string> allowed,
             paging::Pager* pager = nullptr, CancelToken cancel_token = nullptr);

  std::string Name() const override;
  std::string Description() const override;
  boost::json::value ParametersSchema() const override;
  std::string Execute(const boost::json::value& args, pu::ToolContext& ctx) override;

  // The stop of the turn this tool is about to run inside. Set per turn rather than held from
  // construction, because a token belongs to a turn and a toolbox outlives every one of them.
  void SetCancelToken(CancelToken token) { cancel_token_ = std::move(token); }

 private:
  std::string Answer(const std::string& target, const std::string& prompt,
                     const DelegationResult& result, pu::ToolContext& ctx);

  std::filesystem::path root_;
  std::vector<std::string> allowed_;
  // Where an answer is recorded so it can be superseded. Owned by the runtime, which outlives
  // every toolbox this tool is rebuilt into.
  paging::Pager* pager_ = nullptr;
  // The asking turn's stop, which the delegated turn answers to as its own.
  CancelToken cancel_token_;
};

}  // namespace pu::tools
