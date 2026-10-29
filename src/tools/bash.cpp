// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/bash.hpp"

#include "pu/core/json.hpp"
#include "pu/exec/run.hpp"
#include "pu/tools/policy.hpp"

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

namespace pu::tools {

namespace {

std::string Refuse(const std::string& error, bool blocked = false,
                   Refusal refusal = Refusal::kNone) {
  return MakeToolResultJson(false, "", error, -1, blocked, refusal);
}

}  // namespace

BashTool::BashTool(std::string sandbox_root, paging::Pager& pager)
    : sandbox_root_(std::move(sandbox_root)), pager_(pager) {}

std::string BashTool::Name() const { return "execute_bash"; }

std::string BashTool::Description() const {
  return "Run a shell command and read one page of its output.\n"
         "`at` says where to read. 'head' (the default) runs the command and starts at its "
         "first line; 'tail' runs it and starts at its last, which is where a log or a "
         "build's failure is; a cursor like 'o3:240' reads that page of output already "
         "stored and runs nothing, so `command` is not needed with it.\n"
         "Every page carries page.earlier and page.later, the cursors that reach its "
         "neighbours. page.first and page.last are lines of the command's whole output, and "
         "extent says how much of that output the buffer kept: when extent.truncated is "
         "true, the rest of the output cannot be paged to and the command has to be run "
         "again.\n"
         "A page of a run the same command has since replaced comes back with an empty "
         "output and superseded_by naming the newer run; read it again by its cursor if you "
         "still need it.";
}

boost::json::value BashTool::ParametersSchema() const {
  return boost::json::object{
      {"type", "object"},
      {"properties",
       boost::json::object{
           {"command",
            boost::json::object{
                {"type", "string"},
                {"description", "The shell command. Not needed when `at` names a cursor."},
            }},
           {"at",
            boost::json::object{
                {"type", "string"},
                {"description",
                 "Where to read: 'head' (default), 'tail', or a cursor taken from "
                 "page.earlier or page.later."},
            }},
           {"limit",
            boost::json::object{
                {"type", "integer"},
                {"description", "Lines this page shows."},
            }},
       }},
  };
}

std::size_t BashTool::PageLines(const boost::json::value& args) const {
  const paging::Budget budget = pager_.budget();
  std::size_t lines =
      json::OptionalOrDefault<std::size_t>(args, "limit").value_or(budget.page_lines);
  if (lines == 0) lines = budget.page_lines;
  return std::min(lines, budget.max_lines);
}

std::string BashTool::SourceKey(const std::string& command) const {
  // The directory is part of the question: `ls` in two directories asks two different
  // things, and only the result can tell them apart.
  std::string key = command;
  key.push_back('\x1f');
  key.append(sandbox_root_);
  return key;
}

std::string BashTool::PageResult(const paging::Buffer& buffer, const paging::Anchor& anchor,
                                 std::size_t lines) const {
  const paging::Page page = paging::Read(buffer, anchor, lines);
  const exec::Verdict& verdict = buffer.verdict();
  boost::json::object extra = paging::Envelope(buffer, page);
  return MakeToolResultJson(verdict.ok, page.text, verdict.error, verdict.exit_code, false,
                            Refusal::kNone, &extra);
}

std::string BashTool::ReadAt(const paging::Cursor& cursor, std::size_t lines) {
  auto buffer = pager_.Find(cursor.buffer);
  if (!buffer) {
    return Refuse("no such page: " + cursor.buffer +
                  " is no longer retained. Run the command again to read it.");
  }
  if (cursor.line >= buffer->lines()) {
    return Refuse("cursor is past the end of " + cursor.buffer);
  }
  return PageResult(*buffer, paging::Anchor::At(cursor), lines);
}

std::string BashTool::RunAndPage(const std::string& command, const paging::Anchor& anchor,
                                 std::size_t lines, const pu::ToolContext& ctx) {
  const pu::CancelToken stop = ctx.control != nullptr ? ctx.control->stop_token : pu::CancelToken{};
  const paging::Budget budget = pager_.budget();

  auto ran = exec::Run(command, stop, sandbox_root_, budget.capture_bytes,
                       anchor.from_end() ? exec::Keep::kEnd : exec::Keep::kHead);

  // The user's own words beside a stopped command reach the model beside its result.
  if (ran.verdict.stopped && ctx.control != nullptr && !ctx.control->note.empty()) {
    ran.verdict.error += ". The user adds: " + ctx.control->note;
  }

  auto buffer =
      pager_.Store(std::move(ran.captured), std::move(ran.verdict), SourceKey(command));
  return PageResult(*buffer, anchor, lines);
}

std::string BashTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  const std::string at = json::ValueOrDefault<std::string>(args, "at", "head");
  const auto anchor = paging::Anchor::Parse(at);
  if (!anchor) {
    return Refuse("at must be 'head', 'tail', or a cursor from page.earlier/page.later, not '" +
                  at + "'");
  }

  const std::size_t lines = PageLines(args);

  // A page reads output this tool already holds, so it neither re-runs nor re-checks the
  // command: the policy answered for it when the output was produced.
  if (anchor->is_cursor()) return ReadAt(anchor->cursor(), lines);

  std::string command;
  if (args.is_object() && json::HasKey(args, "command")) {
    command = boost::json::value_to<std::string>(args.at("command"));
  }
  if (command.empty()) return Refuse("'command' is required unless 'at' names a cursor");

  if (ctx.security != nullptr && ctx.security->max_command_length > 0 &&
      command.size() > ctx.security->max_command_length) {
    return Refuse("command exceeds maximum allowed length (" +
                  std::to_string(ctx.security->max_command_length) + ")");
  }

  if (ctx.security != nullptr) {
    for (const auto& pattern : ctx.security->forbidden_patterns) {
      try {
        if (std::regex_search(command, std::regex("\\b" + pattern + "\\b"))) {
          return Refuse("Command rejected: contains forbidden pattern '" + pattern +
                            "'. This sandbox forbids " +
                            QuoteJoin(ctx.security->forbidden_patterns) +
                            ". Rework the command to stay within them.",
                        true);
        }
      } catch (const std::regex_error&) {
      }
    }
  }

  const Assessment assessment = Assess(command, ctx.security);
  if (assessment.risk == Risk::kDangerous) {
    return Refuse("Command rejected: " + assessment.reason, true);
  }
  if (assessment.risk == Risk::kApproval) {
    if (ctx.approval == nullptr || !ctx.approval->ask) {
      // Nowhere to ask is a refusal, not a silent run: the policy named this command.
      return Refuse("Command needs user approval and no approval channel is open. Rework it to "
                    "avoid: " +
                        QuoteJoin(ctx.security != nullptr ? ctx.security->approval_patterns
                                                          : std::vector<std::string>{}) +
                        ".",
                    true, Refusal::kNobodyToAsk);
    }
    std::string note;
    if (!ctx.approval->ask(command, note)) {
      std::string message = "Command rejected by the user";
      if (!note.empty()) message += ". The user adds: " + note;
      return Refuse(message, true);
    }
    // The words the user typed beside their yes reach the model beside the result.
    if (!note.empty() && ctx.control != nullptr && ctx.control->on_note) {
      ctx.control->on_note(note);
    }
  }

  return RunAndPage(command, *anchor, lines, ctx);
}

}  // namespace pu::tools
