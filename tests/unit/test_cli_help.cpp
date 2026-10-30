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
  // Both narrow the listing rather than answering about the configuration, so both are on the
  // page that prints it: `--detail caps` adds the columns, `--entry` replaces the list with one
  // model's agents.json entry. `check` keeps only the question it can answer without a runtime.
  REQUIRE(Names(line.HelpFor("models"), "entry"));
  REQUIRE(Names(line.HelpFor("models"), "detail"));
  REQUIRE_FALSE(Names(line.HelpFor("check"), "entry"));
  REQUIRE_FALSE(Names(line.HelpFor("check"), "caps"));
  REQUIRE_FALSE(Names(line.HelpFor("check"), "agent"));
  REQUIRE_FALSE(Names(line.HelpFor("ask"), "verify"));
  REQUIRE_FALSE(Names(line.HelpFor("ask"), "entry"));

  REQUIRE(Names(line.HelpFor("serve"), "bind"));
  REQUIRE(Names(line.HelpFor("serve"), "port"));
  // `host` is a backend's to name; a listener has a `bind`, and the two are not the same word
  // because only one of them is an address a browser can be pointed at.
  REQUIRE_FALSE(Names(line.HelpFor("serve"), "host"));
  REQUIRE_FALSE(Names(line.HelpFor("models"), "bind"));

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

TEST_CASE("The commands named without one are the commands this build has", "[options]") {
  // `pu --help` is asked by a reader who does not know which page to ask for, so it lists
  // the command names. The list is the one `--command` is described from and the one
  // `per_command` is built from, which is what keeps a new command from being named in one
  // place and missing from the other.
  const pu::cli::CommandLine line = pu::cli::BuildCommandLine();

  std::vector<std::string> listed;
  for (const auto& command : line.commands) listed.push_back(command.name);

  std::vector<std::string> described;
  for (const auto& [name, page] : line.per_command) described.push_back(name);

  REQUIRE(listed == described);
}

TEST_CASE("The list says what each command answers", "[options]") {
  const pu::cli::CommandLine line = pu::cli::BuildCommandLine();
  const std::string printed = pu::cli::CommandList(line);

  // A name with no sentence after it is a name a reader still has to look up. A developer tool
  // is named and described but kept off the page, so the two claims are made separately: it has
  // a sentence, and that sentence is not printed.
  std::size_t visible = 0;
  for (const auto& command : line.commands) {
    REQUIRE_FALSE(command.summary.empty());
    const std::size_t at = printed.find("  " + command.name);
    if (command.developer) {
      REQUIRE(at == std::string::npos);
      continue;
    }
    REQUIRE(at != std::string::npos);
    REQUIRE(printed.find(command.summary, at) != std::string::npos);
    ++visible;
  }
  // The listing is not allowed to empty itself: every command becoming a developer tool would
  // satisfy the loop above and leave a reader with a page that says nothing.
  REQUIRE(visible == line.commands.size() - 1);
}

TEST_CASE("A hint belongs to its command's page and nowhere else", "[options]") {
  const pu::cli::CommandLine line = pu::cli::BuildCommandLine();

  // The two commands that read a provider are the ones a reader can pick wrongly, so the
  // page says how they differ. Anything else naming a neighbour would be a page explaining a
  // choice nobody is standing at.
  REQUIRE_FALSE(line.HintFor("probe").empty());
  REQUIRE_FALSE(line.HintFor("sse").empty());
  REQUIRE(line.HintFor("probe").find("agents.json") != std::string::npos);
  REQUIRE(line.HintFor("sse").find("agents.json") != std::string::npos);

  // A hint that named every command would make the two above true for the wrong reason.
  std::size_t hinted = 0;
  for (const auto& command : line.commands) {
    if (!line.HintFor(command.name).empty()) ++hinted;
  }
  REQUIRE(hinted == 2);

  // Nothing outside a command's own page prints it: the top-level listing is written from
  // `summary` alone, and a hint leaking into it would be a second sentence in a one-line map.
  // An empty hint is skipped, since the empty string is found in every line.
  const std::string printed = pu::cli::CommandList(line);
  for (const auto& command : line.commands) {
    const std::string hint = line.HintFor(command.name);
    if (hint.empty()) continue;
    REQUIRE(printed.find(hint) == std::string::npos);
  }

  // A name this build does not have has no page, so it has no hint either.
  REQUIRE(line.HintFor("").empty());
  REQUIRE(line.HintFor("definitely-not-a-command").empty());
}
