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

std::string Page(boost::json::value& j, const char* field) {
  return boost::json::value_to<std::string>(j.at("page").at(field));
}

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
  REQUIRE(j.at("error") == "'command' is required unless 'at' names a cursor");
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

TEST_CASE("execute_bash reads the head of an output one page at a time", "[bash]") {
  ScopedTempDir dir("pu_page");
  const auto file = LinesFile(dir.Path(), 10);

  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  boost::json::value args{{"command", PrintFileCommand(file.string())}};
  auto first = boost::json::parse(tool.Execute(args, ctx));
  REQUIRE(first.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(first.at("output")) == "line 1\nline 2\nline 3\n");

  REQUIRE(first.at("page").at("first") == 1);
  REQUIRE(first.at("page").at("last") == 3);
  REQUIRE(first.at("page").at("earlier").is_null());
  REQUIRE(Page(first, "later") == "o1:3");

  // Three scopes, not one: where the page is, which run it came from, how much survived.
  REQUIRE(first.at("buffer").at("id") == "o1");
  REQUIRE(first.at("extent").at("lines") == 10);
  REQUIRE(first.at("extent").at("retained") == 10);
  REQUIRE(first.at("extent").at("truncated") == false);

  auto second = boost::json::parse(tool.Execute(boost::json::value{{"at", "o1:3"}}, ctx));
  REQUIRE(second.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(second.at("output")) == "line 4\nline 5\nline 6\n");
  REQUIRE(Page(second, "earlier") == "o1:0");
  REQUIRE(Page(second, "later") == "o1:6");
}

TEST_CASE("execute_bash starts at the end when asked to", "[bash]") {
  ScopedTempDir dir("pu_tail");
  const auto file = LinesFile(dir.Path(), 10);
  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(
      boost::json::value{{"command", PrintFileCommand(file.string())}, {"at", "tail"}}, ctx));

  REQUIRE(boost::json::value_to<std::string>(j.at("output")) == "line 8\nline 9\nline 10\n");
  REQUIRE(j.at("page").at("first") == 8);
  REQUIRE(j.at("page").at("last") == 10);
  REQUIRE(Page(j, "earlier") == "o1:4");
  REQUIRE(j.at("page").at("later").is_null());
}

TEST_CASE("execute_bash clamps a page to the configured ceiling", "[bash]") {
  ScopedTempDir dir("pu_limit");
  const auto file = LinesFile(dir.Path(), 10);
  paging::Pager pager(SmallPages());
  BashTool tool(dir.Path().string(), pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(
      boost::json::value{{"command", PrintFileCommand(file.string())}, {"limit", 100}}, ctx));

  REQUIRE(j.at("page").at("last") == 5);
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
  const std::string cursor = Page(first, "later");

  // The cursor names the run; restating a 200-character command to read page two is a
  // second source of truth that can only disagree.
  auto second = boost::json::parse(tool.Execute(boost::json::value{{"at", cursor}}, ctx));
  REQUIRE(second.at("success") == true);
  REQUIRE(boost::json::value_to<std::string>(second.at("output")) ==
          "line 4\nline 5\nline 6\n");
}

TEST_CASE("a cursor for a buffer the store dropped is refused", "[bash]") {
  paging::Pager pager(SmallPages());
  BashTool tool(".", pager);
  ToolContext ctx;

  auto j = boost::json::parse(tool.Execute(boost::json::value{{"at", "o99:0"}}, ctx));
  REQUIRE(j.at("success") == false);
  REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("no longer retained") !=
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
  REQUIRE(j.at("extent").at("truncated") == true);
  REQUIRE(j.at("extent").at("lines") == 200);
  REQUIRE(boost::json::value_to<std::size_t>(j.at("extent").at("retained")) < 200);
  REQUIRE(j.at("page").at("first") == 1);
  // The rest of the output is not reachable, and the extent is what says so: paging
  // stops at extent.retained rather than at extent.lines.
  REQUIRE(j.at("extent").at("truncated") == true);
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
  REQUIRE(j.at("extent").at("truncated") == true);
  REQUIRE(j.at("extent").at("lines") == 200);
  // A tail of a log that did not fit is still a tail of that log.
  REQUIRE(j.at("page").at("last") == 200);
  REQUIRE(boost::json::value_to<std::size_t>(j.at("page").at("first")) > 1);
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
  const std::string cursor = Page(first, "later");

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
  policy.sandbox_root = "/tmp/safe";
  ToolContext ctx;
  ctx.security = &policy;

  for (const char* path : {"../etc/passwd", "/tmp/safe-evil/x"}) {
    auto j = boost::json::parse(
        tool.Execute(boost::json::value{{"path", path}, {"content", "evil"}}, ctx));
    REQUIRE(j.at("success") == false);
    REQUIRE(boost::json::value_to<std::string>(j.at("error")).find("traversal") !=
            std::string::npos);
  }
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
