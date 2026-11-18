// SPDX-License-Identifier: GPL-3.0-only
#include "pu/tools/bash.hpp"

#include "pu/core/json.hpp"
#include "pu/core/process.hpp"
#include "pu/tools/policy.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace pu::tools {

BashTool::BashTool(std::string sandbox_root, paging::Pager& pager)
    : sandbox_root_(std::move(sandbox_root)), pager_(pager) {}

std::string BashTool::Name() const { return "execute_bash"; }

std::string BashTool::Description() const {
  return "Run a shell command, or read more of the last command's output.\n"
         "Running: omit `at` to start at the last line, which is where a log's outcome or a "
         "build's failure is; set it to 'head' to start at the first line instead.\n"
         "Output too long to show is kept, and you can read the rest without running "
         "anything: call this tool again with `at` set to a line number to see those lines, "
         "or with `query` set to a word to get the lines that contain it. A result says how "
         "many lines and bytes the command printed and how many were dropped, so you can "
         "tell when the part you want is gone rather than merely further down.\n"
         "Reading never re-runs the command and never replaces what is stored. Only running "
         "a new command does that, so read as much of one command's output as you need "
         "before running the next.\n"
         "A result of a command that a later run of the same command has replaced comes back "
         "empty, with `superseded_by` naming the newer run.";
}

boost::json::value BashTool::ParametersSchema() const {
  return boost::json::object{
      {"type", "object"},
      {"properties",
       boost::json::object{
           {"command",
            boost::json::object{
                {"type", "string"},
                {"description", "The shell command. Not needed when reading stored output."},
            }},
           {"at",
            boost::json::object{
                {"type", "string"},
                {"description",
                 "Omit to run the command and start at its last line, or set 'head' to start at "
                 "the first. A line number reads that line of the last command's output without "
                 "running it."},
            }},
           {"query",
            boost::json::object{
                {"type", "string"},
                {"description",
                 "Find the lines of the last command's output containing this text. Reads "
                 "only; runs nothing."},
            }},
           {"context",
            boost::json::object{
                {"type", "integer"},
                {"description", "Lines shown either side of a `query` match."},
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
  const paging::Page page = paging::Read(buffer, anchor, lines, pager_.budget().page_bytes);
  const core::Verdict& verdict = buffer.verdict();
  boost::json::object extra = paging::Envelope(buffer, page);
  return MakeToolResultJson(verdict.ok, page.text, verdict.error, verdict.exit_code, false,
                            Refusal::kNone, &extra);
}

std::string BashTool::ReadAt(const paging::Cursor& cursor, std::size_t lines) {
  auto buffer = pager_.Find(cursor.buffer);
  if (!buffer) {
    // The id is not repeated. It was never the model's to know, and naming it in a refusal
    // invites the model to try again with a variation on a string it should never have held.
    return MakeRefusal("that output is no longer stored. Run the command again to read it.");
  }
  if (cursor.line >= buffer->lines()) {
    return MakeRefusal("line " + std::to_string(cursor.line + 1) + " is past the end of the " +
                       "stored output, which has " + std::to_string(buffer->lines()) +
                       " lines.");
  }
  return PageResult(*buffer, paging::Anchor::At(cursor), lines);
}

std::string BashTool::RunAndPage(const std::string& command, const paging::Anchor& anchor,
                                 std::size_t lines, const pu::ToolContext& ctx) {
  const pu::CancelToken stop = ctx.control != nullptr ? ctx.control->stop_token : pu::CancelToken{};
  const paging::Budget budget = pager_.budget();

  auto ran = core::Run(command, stop, sandbox_root_, budget.capture_bytes,
                       anchor.from_end() ? core::Keep::kEnd : core::Keep::kHead);

  // The user's own words beside a stopped command reach the model beside its result.
  if (ran.verdict.stopped && ctx.control != nullptr && !ctx.control->note.empty()) {
    ran.verdict.error += ". The user adds: " + ctx.control->note;
  }

  auto buffer =
      pager_.Store(std::move(ran.captured), std::move(ran.verdict), SourceKey(command));
  return PageResult(*buffer, anchor, lines);
}

std::string BashTool::SearchStoredRun(std::string_view needle, std::size_t context,
                                      std::size_t max_lines) const {
  const std::shared_ptr<const paging::Buffer> buffer = pager_.Newest();
  if (!buffer) {
    return MakeRefusal("there is no stored output to search. Run a command first.");
  }

  const paging::Matches matches = paging::Search(*buffer, needle, context, max_lines);
  const core::Verdict& verdict = buffer->verdict();

  boost::json::object extra = paging::Envelope(*buffer, paging::Page{});
  // The page bounds a text read; a search has no page, so it reports its own counts and
  // leaves `lines` naming the stored run as a whole. Without this a search result would
  // claim to be showing lines 0 to 0.
  extra["matches"] = boost::json::object{
      {"found", matches.count},
      {"shown", matches.shown},
      {"truncated", matches.truncated},
  };
  if (matches.count == 0) {
    return MakeToolResultJson(
        verdict.ok,
        "No line of the stored output contains '" + std::string(needle) + "'. The output that "
        "was dropped is not searchable; if the text you want may be in it, run the command "
        "again with a narrower one, such as a pipeline through grep.",
        verdict.error, verdict.exit_code, false, Refusal::kNone, &extra);
  }
  return MakeToolResultJson(verdict.ok, matches.text, verdict.error, verdict.exit_code, false,
                            Refusal::kNone, &extra);
}

std::string BashTool::Execute(const boost::json::value& args, pu::ToolContext& ctx) {
  const std::size_t lines = PageLines(args);

  // A search is a read. It happens before `at` is parsed, because `query` alone -- which is
  // how a model asks "is it in there?" -- carries no `at` and would otherwise be read as a
  // fresh run with no command.
  const std::string query = json::ValueOrDefault<std::string>(args, "query", "");
  if (!query.empty()) {
    const std::size_t context =
        json::OptionalOrDefault<std::size_t>(args, "context").value_or(2);
    return SearchStoredRun(query, context, lines);
  }

  // A command's interesting end is its last line: a failure is reported there, and a build
  // log is read for the error that stopped it rather than the progress before it. So the
  // first look is the tail, and `at: "head"` is the deliberate request for the other end.
  // A command that printed little enough to fit is unaffected -- there is only one page.
  const std::string at = json::ValueOrDefault<std::string>(args, "at", "tail");
  const auto anchor = paging::Anchor::Parse(at);
  if (!anchor) {
    return MakeRefusal("at must be 'head', 'tail', or a line number of the last command's "
                  "output, not '" +
                  at + "'");
  }

  // A page reads output this tool already holds, so it neither re-runs nor re-checks the
  // command: the policy answered for it when the output was produced.
  if (anchor->is_cursor()) return ReadAt(anchor->cursor(), lines);

  std::string command;
  if (args.is_object() && json::HasKey(args, "command")) {
    command = boost::json::value_to<std::string>(args.at("command"));
  }
  if (command.empty()) {
    return MakeRefusal("'command' is required to run something. Set `at` to a line number or "
                       "`query` to search only if you meant to read the last output.");
  }

  if (ctx.security != nullptr && ctx.security->max_command_length > 0 &&
      command.size() > ctx.security->max_command_length) {
    return MakeRefusal("command exceeds maximum allowed length (" +
                  std::to_string(ctx.security->max_command_length) + ")");
  }

  if (ctx.security != nullptr && !ctx.security->forbidden_patterns.empty() &&
      MatchesAnyPattern(command, ctx.security->forbidden_patterns)) {
    return MakeRefusal("Command rejected: contains a forbidden pattern. This sandbox forbids " +
                           QuoteJoin(ctx.security->forbidden_patterns) +
                           ". Rework the command to stay within them.",
                       true);
  }

  const Assessment assessment = Assess(command, ctx.security);
  if (assessment.risk == Risk::kDangerous) {
    return MakeRefusal("Command rejected: " + assessment.reason, true);
  }
  if (assessment.risk == Risk::kApproval) {
    if (ctx.approval == nullptr || !ctx.approval->ask) {
      // Nowhere to ask is a refusal, not a silent run: the policy named this command.
      return MakeRefusal("Command needs user approval and no approval channel is open. Rework it to "
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
      return MakeRefusal(message, true);
    }
    // The words the user typed beside their yes reach the model beside the result.
    if (!note.empty() && ctx.control != nullptr && ctx.control->on_note) {
      ctx.control->on_note(note);
    }
  }

  return RunAndPage(command, *anchor, lines, ctx);
}

}  // namespace pu::tools
