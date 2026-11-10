// SPDX-License-Identifier: GPL-3.0-only
#include "pu/paging/paging.hpp"
#include "pu/tools/bash.hpp"
#include "pu/tools/write_file.hpp"
#include "tests/mocks/test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <boost/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace pu;
using namespace pu::tools;
using namespace pu::tests;

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return content;
}

// A file of `count` known lines, so a page can be compared exactly.
std::filesystem::path LinesFile(const std::filesystem::path& dir, std::size_t count) {
  const auto path = dir / "lines.txt";
  std::ofstream out(path);
  for (std::size_t i = 1; i <= count; ++i) out << "line " << i << "\n";
  return path;
}

paging::Budget SmallPages() {
  paging::Budget budget;
  budget.page_lines = 3;
  budget.max_lines = 5;
  budget.capture_bytes = 65536;
  return budget;
}

paging::Budget TinyCapture() {
  paging::Budget budget = SmallPages();
  budget.capture_bytes = 256;
  return budget;
}

// Pages of three lines each, but only 16 bytes: enough for two of the seven-byte lines this
// file writes and not for a third, so a byte bound can be seen cutting a page short.
paging::Budget NarrowPages() {
  paging::Budget budget = SmallPages();
  budget.page_bytes = 16;
  return budget;
}

// The id of the run a receipt came from. Tests read it back rather than spelling it, because
// the id also names the pager that wrote it and no test should have to know that mark.
std::string IdOf(const boost::json::value& j) {
  return boost::json::value_to<std::string>(j.at("buffer").at("id"));
}

// A line number of the stored run, as the tool takes it back. This is the whole of what a
// model is told: a line, not a buffer and a line.
std::string LineRef(std::size_t line) { return std::to_string(line); }

}  // namespace

TEST_CASE("execute_bash reports success for a command that worked", "[bash]") {
  paging::Pager pager;
  BashTool tool(".", pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(boost::json::value{{"command", "echo hello"}}, ctx));

  REQUIRE(j.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(j.at("output")).find("hello") != std::string::npos);
  REQUIRE(j.at("blocked") == false);
  REQUIRE(j.at("error") == "");
  REQUIRE(j.at("exit_code") == 0);
}

TEST_CASE("execute_bash reports a failing command's code and words", "[bash]") {
  paging::Pager pager;
  BashTool tool(".", pager);
  ToolContext ctx;

  auto j = boost::json::parse(
      tool.Execute(boost::json::value{{"command", "nonexistent_command_pu_test"}}, ctx));

  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("Command failed") !=
          std::string::npos);
  REQUIRE(j.at("exit_code") != 0);
}

TEST_CASE("execute_bash needs a command unless it is reading a page", "[bash]") {
  paging::Pager pager;
  BashTool tool(".", pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(boost::json::value(boost::json::object{}), ctx));

  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("output") == "");
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("'command' is required") !=
          std::string::npos);
  REQUIRE(j.at("exit_code") == -1);
}

TEST_CASE("execute_bash blocks a forbidden pattern", "[bash]") {
  paging::Pager pager;
  BashTool tool(".", pager);
  config::SecurityPolicy policy;
  policy.forbidden_patterns = {"rm -rf", "sudo"};
  ToolContext ctx;
  ctx.security = &policy;

  auto j = boost::json::parse(
      tool.Execute(boost::json::value{{"command", "sudo rm -rf /"}}, ctx));

  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("forbidden pattern") !=
          std::string::npos);
}

TEST_CASE("execute_bash blocks a dangerous command with no policy to consult", "[bash]") {
  paging::Pager pager;
  BashTool tool(".", pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(boost::json::value{{"command", "rm -rf /"}}, ctx));

  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("Command rejected:") !=
          std::string::npos);
  REQUIRE(j.at("blocked") == true);
}

TEST_CASE("execute_bash enforces the maximum command length", "[bash]") {
  paging::Pager pager;
  BashTool tool(".", pager);
  config::SecurityPolicy policy;
  policy.max_command_length = 10;
  ToolContext ctx;
  ctx.security = &policy;

  auto j = boost::json::parse(tool.Execute(
      boost::json::value{{"command", "echo this is way too long for the limit"}}, ctx));

  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("exceeds maximum allowed length") !=
          std::string::npos);
}

TEST_CASE("execute_bash reads the end of an output one page at a time", "[bash]") {
  ScopedTempDir dir("pu_page");
  const auto file = LinesFile(dir.Path(), 10);

  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  // Nobody reads a log from its first line: a command that printed ten lines and one that
  // printed ten thousand both have their outcome at the end. So the first look is the tail.
  boost::json::value args{{"command", PrintFileCommand(file.string())}};
  auto first = boost::json::parse(tool.Execute(args, ctx));
  REQUIRE(first.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(first.at("output")) ==
          "line 8\nline 9\nline 10\n");

  REQUIRE(first.at("lines").at("first") == 8);
  REQUIRE(first.at("lines").at("last") == 10);
  REQUIRE(first.at("lines").at("of") == 10);

  // The run is named for the machine, and nothing else is.
  REQUIRE(first.at("buffer").at("rank") == 1);
  REQUIRE(first.at("stored").at("printed") == 10);
  REQUIRE(first.at("stored").at("kept") == 10);
  REQUIRE(first.at("stored").at("dropped") == 0);
  // Both counts are reported: the line count is what a follow-up read names, the byte count
  // is what tells a model how much of its context the command spent.
  REQUIRE(first.at("stored").at("printed_bytes").as_int64() > 0);
  REQUIRE(first.at("stored").at("kept_bytes") == first.at("stored").at("printed_bytes"));
  REQUIRE(first.at("read_more").is_null());

  // A line number reads the same stored output, and the run is the newest one because a
  // bare number cannot name another.
  auto second = boost::json::parse(tool.Execute(boost::json::value{{"at", LineRef(4)}}, ctx));
  REQUIRE(second.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(second.at("output")) == "line 4\nline 5\nline 6\n");
  REQUIRE(second.at("lines").at("first") == 4);
}

TEST_CASE("execute_bash bounds a page by bytes as well as by lines", "[bash]") {
  ScopedTempDir dir("pu_page_bytes");
  const auto file = LinesFile(dir.Path(), 10);

  paging::Pager pager(NarrowPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(
      boost::json::value{{"command", PrintFileCommand(file.string())}, {"at", "head"}}, ctx));

  REQUIRE(j.at("success") == true);
  // Three lines were asked for, but two seven-byte lines are 14 bytes and a third would take
  // the page past 16. Lines alone would let a page grow without bound on wide output, which
  // is the case a byte bound exists for.
  REQUIRE(boost::json::value_to<std::string>(j.at("output")) == "line 1\nline 2\n");
  REQUIRE(j.at("lines").at("first") == 1);
  REQUIRE(j.at("lines").at("last") == 2);
  REQUIRE(j.at("lines").at("of") == 10);

  // A model that wants the rest asks from the line after the last one it got.
  auto next = boost::json::parse(tool.Execute(boost::json::value{{"at", LineRef(3)}}, ctx));
  REQUIRE(next.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(next.at("output")).rfind("line 3\n", 0) == 0);
}

TEST_CASE("a page shows one line even when that line exceeds the byte bound", "[bash]") {
  ScopedTempDir dir("pu_page_wide");
  const auto file = LinesFile(dir.Path(), 10);

  // A single line is the smallest page there is: an empty page would be indistinguishable
  // from a command that printed nothing.
  paging::Budget tight = NarrowPages();
  tight.page_bytes = 1;
  paging::Pager pager(tight);
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(
      boost::json::value{{"command", PrintFileCommand(file.string())}, {"at", "head"}}, ctx));
  REQUIRE(j.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(j.at("output")) == "line 1\n");
  REQUIRE(j.at("lines").at("first") == 1);
  REQUIRE(j.at("lines").at("last") == 1);
}

TEST_CASE("execute_bash starts at the beginning when asked to", "[bash]") {
  ScopedTempDir dir("pu_tail");
  const auto file = LinesFile(dir.Path(), 10);
  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(
      boost::json::value{{"command", PrintFileCommand(file.string())}, {"at", "head"}}, ctx));

  REQUIRE(boost::json::value_to<std::string>(j.at("output")) == "line 1\nline 2\nline 3\n");
  REQUIRE(j.at("lines").at("first") == 1);
  REQUIRE(j.at("lines").at("last") == 3);
  REQUIRE(j.at("lines").at("of") == 10);
}

TEST_CASE("execute_bash clamps a page to the configured ceiling", "[bash]") {
  ScopedTempDir dir("pu_limit");
  const auto file = LinesFile(dir.Path(), 10);
  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(boost::json::value{{"command", PrintFileCommand(file.string())},
                                            {"at", "head"},
                                            {"limit", 100}},
                                           ctx));

  REQUIRE(j.at("lines").at("last") == 5);
}

TEST_CASE("execute_bash refuses an `at` it does not define", "[bash]") {
  paging::Pager pager(SmallPages());
  BashTool tool(".", pager);
  ToolContext ctx;

  auto j = boost::json::parse(
      tool.Execute(boost::json::value{{"command", "echo hello"}, {"at", "middle"}}, ctx));

  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("at must be") !=
          std::string::npos);
}

TEST_CASE("a cursor reads a page with no command to repeat", "[bash]") {
  ScopedTempDir dir("pu_cursor");
  const auto file = LinesFile(dir.Path(), 10);
  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto first = boost::json::parse(
      tool.Execute(boost::json::value{{"command", PrintFileCommand(file.string())}}, ctx));
  const std::string cursor = LineRef(4);

  // A line number names the newest run; restating a 200-character command to read page two
  // is a second source of truth that can only disagree.
  auto second = boost::json::parse(tool.Execute(boost::json::value{{"at", cursor}}, ctx));
  REQUIRE(second.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(second.at("output")) ==
          "line 4\nline 5\nline 6\n");
}

TEST_CASE("a line number with no stored run is refused", "[bash]") {
  paging::Pager pager(SmallPages());
  BashTool tool(".", pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(boost::json::value{{"at", LineRef(1)}}, ctx));
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("no longer stored") !=
          std::string::npos);
}

TEST_CASE("a query finds lines of the stored output without running anything", "[bash]") {
  ScopedTempDir dir("pu_query");
  const auto file = LinesFile(dir.Path(), 40);
  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto first = boost::json::parse(
      tool.Execute(boost::json::value{{"command", PrintFileCommand(file.string())}}, ctx));
  const std::string id_before = IdOf(first);

  // "line 7" matches line 7 alone; "line 3" would match 3, 30-39, so the needle is chosen
  // to be unambiguous and the counts mean one thing.
  auto found = boost::json::parse(
      tool.Execute(boost::json::value{{"query", "line 7"}, {"context", 1}}, ctx));
  REQUIRE(found.at("success") == true);
  REQUIRE(found.at("matches").at("found") == 1);
  REQUIRE(found.at("matches").at("shown") == 1);
  REQUIRE(found.at("matches").at("truncated") == false);

  // Reported with the command's own line numbers, so the hit can be followed up by `at`.
  const std::string text = boost::json::value_to<std::string>(found.at("output"));
  REQUIRE(text.find("6:line 6") != std::string::npos);
  REQUIRE(text.find("7:line 7") != std::string::npos);
  REQUIRE(text.find("8:line 8") != std::string::npos);

  // A search is a read: the run it searched is still the run the store holds.
  REQUIRE(IdOf(boost::json::parse(
              tool.Execute(boost::json::value{{"at", LineRef(1)}}, ctx))) == id_before);
}

TEST_CASE("a query that finds nothing says so without replacing the output", "[bash]") {
  ScopedTempDir dir("pu_query_miss");
  const auto file = LinesFile(dir.Path(), 10);
  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto first = boost::json::parse(
      tool.Execute(boost::json::value{{"command", PrintFileCommand(file.string())}}, ctx));

  auto miss = boost::json::parse(tool.Execute(boost::json::value{{"query", "nowhere"}}, ctx));
  REQUIRE(miss.at("success") == true);
  REQUIRE(miss.at("matches").at("found") == 0);
  REQUIRE(boost::json::value_to<std::string>(miss.at("output")).find("dropped") !=
          std::string::npos);

  // The stored run survives a search that found nothing, so a miss costs a turn and nothing
  // else.
  auto again = boost::json::parse(tool.Execute(boost::json::value{{"at", LineRef(1)}}, ctx));
  REQUIRE(again.at("success") == true);
  REQUIRE(IdOf(again) == IdOf(first));
}

TEST_CASE("a query with no stored output is refused", "[bash]") {
  paging::Pager pager(SmallPages());
  BashTool tool(".", pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(boost::json::value{{"query", "error"}}, ctx));
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("Run a command first") !=
          std::string::npos);
}

TEST_CASE("execute_bash reports what a truncated capture did not keep", "[bash]") {
  ScopedTempDir dir("pu_truncated");
  const auto file = LinesFile(dir.Path(), 200);

  paging::Pager pager(TinyCapture());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto j = boost::json::parse(
      tool.Execute(boost::json::value{{"command", PrintFileCommand(file.string())}}, ctx));

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("stored").at("printed") == 200);
  REQUIRE(boost::json::value_to<std::size_t>(j.at("stored").at("kept")) < 200);
  REQUIRE(boost::json::value_to<std::size_t>(j.at("stored").at("dropped")) > 0);
  // The kept lines are the end of the output, because that is the end a first look reads and
  // therefore the end worth spending the capture on.
  REQUIRE(j.at("lines").at("last") == 200);
  // The dropped lines are named, so a model knows the text it wants may be gone rather than
  // further down, and is told how to look anyway.
  REQUIRE(boost::json::value_to<std::string>(j.at("stored").at("kept_from")) == "the end");
  REQUIRE_FALSE(j.at("read_more").is_null());
}

TEST_CASE("execute_bash spends the budget on the end it was asked to read", "[bash]") {
  ScopedTempDir dir("pu_truncated_tail");
  const auto file = LinesFile(dir.Path(), 200);

  paging::Pager pager(TinyCapture());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(
      boost::json::value{{"command", PrintFileCommand(file.string())}, {"at", "tail"}}, ctx));

  REQUIRE(j.at("success") == true);
  REQUIRE(j.at("stored").at("printed") == 200);
  // A tail of a log that did not fit is still a tail of that log.
  REQUIRE(j.at("lines").at("last") == 200);
  REQUIRE(boost::json::value_to<std::size_t>(j.at("lines").at("first")) > 1);
  REQUIRE(boost::json::value_to<std::string>(j.at("stored").at("kept_from")) == "the end");
  REQUIRE(boost::json::value_to<std::string>(j.at("output")).find("line 200") !=
          std::string::npos);
}

TEST_CASE("Two runs of one command name one source and two buffers", "[bash]") {
  ScopedTempDir dir("pu_source");
  const auto file = LinesFile(dir.Path(), 6);
  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  boost::json::value args{{"command", PrintFileCommand(file.string())}};
  auto first = boost::json::parse(tool.Execute(args, ctx));
  auto second = boost::json::parse(tool.Execute(args, ctx));

  // Same question, two answers: the projection replaces the older run of a source and
  // needs the run to stay separable.
  REQUIRE(first.at("buffer").at("source") == second.at("buffer").at("source"));
  REQUIRE(first.at("buffer").at("id") != second.at("buffer").at("id"));
}

TEST_CASE("A cursor still pages after the tool is rebuilt on the same store", "[bash]") {
  ScopedTempDir dir("pu_shared_store");
  const auto file = LinesFile(dir.Path(), 10);
  paging::Pager pager(SmallPages());
  ToolContext ctx;

  auto first = boost::json::parse(BashTool(dir.Path().string(), pager)
                                      .Execute(boost::json::value{{"command",
                                                                   PrintFileCommand(file.string())}},
                                               ctx));
  const std::string cursor = LineRef(4);

  // A model switch rebuilds the toolbox: a fresh tool object, the store it was given.
  auto second = boost::json::parse(
      BashTool(dir.Path().string(), pager).Execute(boost::json::value{{"at", cursor}}, ctx));

  REQUIRE(second.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(second.at("output")) == "line 4\nline 5\nline 6\n");
}

TEST_CASE("write_file writes inside the sandbox", "[bash]") {
  std::filesystem::path tmpdir = UniqueTempPath("pu_write_test");
  std::filesystem::create_directories(tmpdir);

  WriteFileTool tool;
  config::SecurityPolicy policy;
  policy.sandbox_root = tmpdir.string();
  ToolContext ctx;
  ctx.security = &policy;

  auto j = boost::json::parse(
      tool.Execute(boost::json::value{{"path", "hello.txt"}, {"content", "Hello, world!"}}, ctx));

  REQUIRE(j.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(j.at("output")).find("Successfully wrote") !=
          std::string::npos);
  REQUIRE(j.at("error") == "");
  REQUIRE(j.at("exit_code") == 0);
  REQUIRE(ReadFile((tmpdir / "hello.txt").string()) == "Hello, world!");

  std::filesystem::remove_all(tmpdir);
}

TEST_CASE("write_file needs a path", "[bash]") {
  WriteFileTool tool;
  config::SecurityPolicy policy;
  policy.sandbox_root = "/tmp";
  ToolContext ctx;
  ctx.security = &policy;

  auto j = boost::json::parse(tool.Execute(boost::json::value{{"content", "some content"}}, ctx));

  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "'path' is required");
  REQUIRE(j.at("exit_code") == -1);
}

TEST_CASE("write_file refuses a path that only looks like the sandbox", "[bash]") {
  WriteFileTool tool;
  config::SecurityPolicy policy;
  // Built from the temp directory rather than spelled '/tmp/safe'. On Windows a leading '/'
  // is a drive-relative root, so '/tmp/safe-evil/x' would not be the sibling name this case
  // is about but an absolute path off whichever drive is current.
  //
  // Canonicalized the way the tool canonicalizes it, because the temp directory a machine
  // hands back is not always the name the filesystem resolves it to: Windows returns an 8.3
  // short path (`RUNNER~1`) where the resolved directory is the long one. The refusal names
  // the resolved root, so the expectation has to be resolved the same way rather than being
  // the string this test happened to build.
  std::error_code ec;
  const std::filesystem::path sandbox = std::filesystem::weakly_canonical(
      std::filesystem::temp_directory_path() / "pu-safe", ec);
  REQUIRE_FALSE(ec);
  policy.sandbox_root = sandbox.string();
  ToolContext ctx;
  ctx.security = &policy;

  const std::string sibling = (sandbox.parent_path() / "pu-safe-evil").string() + "/x";
  for (const std::string& path : {std::string("../etc/passwd"), sibling}) {
    auto j = boost::json::parse(
        tool.Execute(boost::json::value{{"path", path}, {"content", "evil"}}, ctx));
    REQUIRE(j.at("success") == false);
    const std::string error = boost::json::value_to<std::string>(j.at("error"));
    REQUIRE(error.find("traversal") != std::string::npos);
    // The root is the half of the answer a model cannot compute on its own: it has only the
    // path it wrote, and no way to tell which directory that path was measured from.
    REQUIRE(error.find(sandbox.string()) != std::string::npos);
  }
}

TEST_CASE("write_file's page says a dot-directory is an ordinary path", "[bash]") {
  // The one directory a model is most often asked to create for a sub-agent is a `.pu/`, and
  // nothing about "write text to a file" says whether that is in scope. The page answers it
  // before the call, which is cheaper than the turn spent being refused.
  WriteFileTool tool;
  const std::string description = tool.Description();
  REQUIRE(description.find(".pu/") != std::string::npos);
  REQUIRE(description.find("sandbox") != std::string::npos);
}

TEST_CASE("write_file refuses to run with no policy", "[bash]") {
  WriteFileTool tool;
  ToolContext ctx;

  auto j = boost::json::parse(
      tool.Execute(boost::json::value{{"path", "test.txt"}, {"content", "data"}}, ctx));

  REQUIRE(j.at("success") == false);
  REQUIRE(j.at("error") == "security policy not set");
}

TEST_CASE("A refusal says which kind it is", "[tools][refusal]") {
  // What a policy refusal looks like: the flag alone, since the command is what to rework.
  const pu::tools::ToolResult policy = pu::tools::ParseToolResult(
      R"({"success":false,"blocked":true,"output":"","error":"contains forbidden pattern","exit_code":-1})");
  REQUIRE(policy.valid);
  REQUIRE(policy.blocked);
  REQUIRE(policy.refusal == pu::tools::Refusal::kPolicy);

  // What an agent with approval_patterns and no channel to ask on looks like. Same flag,
  // different remedy: nothing is wrong with the command.
  const pu::tools::ToolResult unaskable = pu::tools::ParseToolResult(pu::tools::MakeToolResultJson(
      false, "", "needs approval and no channel is open", -1, true,
      pu::tools::Refusal::kNobodyToAsk));
  REQUIRE(unaskable.valid);
  REQUIRE(unaskable.blocked);
  REQUIRE(unaskable.refusal == pu::tools::Refusal::kNobodyToAsk);

  // A result that ran carries no refusal at all, and one that failed on its own is not a
  // refusal either.
  REQUIRE(pu::tools::ParseToolResult(
              R"({"success":true,"blocked":false,"output":"a.txt","error":"","exit_code":0})")
              .refusal == pu::tools::Refusal::kNone);
  REQUIRE(pu::tools::ParseToolResult(
              R"({"success":false,"blocked":false,"output":"","error":"exit 1","exit_code":1})")
              .refusal == pu::tools::Refusal::kNone);
}
