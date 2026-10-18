// SPDX-License-Identifier: GPL-3.0-only

//
// What `pu <cmd> --help` describes, and what a flag that command does not have does.
//
// The whole line is what the parser sees, because which command was typed is only known after
// parsing. That made every command accept every flag, and `--help` print one page for five
// commands. These cases hold the two apart: a page names its own flags, and a flag from
// another command is refused before anything runs.
//

#include <catch2/catch_test_macros.hpp>

#include <boost/program_options.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "pu/app/cli.hpp"

namespace po = boost::program_options;

namespace {

// The long names a page prints, as the reader sees them.
std::vector<std::string> OptionNames(const po::options_description& page) {
  std::ostringstream printed;
  printed << page;
  std::vector<std::string> names;
  std::istringstream lines(printed.str());
  std::string line;
  while (std::getline(lines, line)) {
    const std::size_t open = line.find("--");
    if (open == std::string::npos) continue;
    const std::size_t end = line.find_first_of(" =", open + 2);
    names.push_back(line.substr(open + 2, end - (open + 2)));
  }
  return names;
}

bool Names(const po::options_description& page, const std::string& name) {
  const std::vector<std::string> names = OptionNames(page);
  return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

TEST_CASE("A subcommand's page names that command's flags", "[options]") {
  const pu::cli::CommandLine line = pu::cli::BuildCommandLine();

  // Each of these reads something the others do not; if every page were the same, the
  // command a reader typed would still not be the one the page described.
  REQUIRE(Names(line.HelpFor("models"), "verify"));
  REQUIRE(Names(line.HelpFor("models"), "entry"));
  REQUIRE_FALSE(Names(line.HelpFor("ask"), "verify"));
  REQUIRE_FALSE(Names(line.HelpFor("ask"), "entry"));

  REQUIRE(Names(line.HelpFor("serve"), "host"));
  REQUIRE(Names(line.HelpFor("serve"), "port"));
  REQUIRE_FALSE(Names(line.HelpFor("models"), "host"));

  REQUIRE(Names(line.HelpFor("check"), "config"));
  REQUIRE_FALSE(Names(line.HelpFor("chat"), "config"));

  // The two answered before any command runs, and the flag that asked for the page, stay on
  // every one of them.
  for (const char* command : {"ask", "chat", "models", "probe", "check", "serve"}) {
    INFO(command);
    const po::options_description& page = line.HelpFor(command);
    REQUIRE(Names(page, "help"));
    REQUIRE(Names(page, "version"));
    // The positional that named the command is not a flag the command takes.
    REQUIRE_FALSE(Names(page, "command"));
  }
}

TEST_CASE("An option every page shares is declared once", "[options]") {
  // boost refuses a description that holds a name twice ("option '--help' is ambiguous"), so
  // the pages are copies of the objects `all` declared rather than fresh declarations.
  const pu::cli::CommandLine line = pu::cli::BuildCommandLine();

  int agent_declarations = 0;
  for (const auto& option : line.all.options()) {
    if (option->long_name() == "agent") ++agent_declarations;
  }
  REQUIRE(agent_declarations == 1);

  // And the page that prints it reads the same object, which is what keeps the help text and
  // the parser from disagreeing about a flag's wording.
  REQUIRE(Names(line.HelpFor("ask"), "agent"));
  REQUIRE(Names(line.HelpFor("models"), "agent"));
}

TEST_CASE("A command this build does not have gets the whole line", "[options]") {
  const pu::cli::CommandLine line = pu::cli::BuildCommandLine();
  // Nothing to narrow to, and printing an empty page would be worse than printing all of it.
  REQUIRE(&line.HelpFor("no-such-command") == &line.all);
}
