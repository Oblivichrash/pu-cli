// SPDX-License-Identifier: GPL-3.0-only
#include "pu/app/cli.hpp"

#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "pu/config/agents.hpp"
#include "pu/model/backend.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime/command_router.hpp"
#include "pu/llm/model_check.hpp"
#include "pu/llm/model_listing.hpp"
#include "pu/llm/field_capture.hpp"
#include "pu/llm/model_factory.hpp"
#include "pu/runtime/runtime.hpp"

#include "app/cli_output.hpp"

namespace pu::cli {

int WithInitializedRuntime(const std::string& agent, Runtime& runtime,
                           const std::function<int()>& body, bool ephemeral) {
  try {
    if (!agent.empty()) runtime.SetDefaultAgent(agent);
    // Before Initialize, which is where a stored conversation is read: a flag set afterwards
    // would arrive after the one decision it was meant to change.
    if (ephemeral) runtime.SetEphemeral();
    runtime.Initialize();
    const int status = body();
    runtime.Shutdown();
    return status;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return static_cast<int>(ExitCode::kUsage);
  }
}

CommandLine BuildCommandLine() {
  // The names, declared once. A description cannot hold a name twice — boost refuses the
  // whole line with "option '--help' is ambiguous" — so `all` is the one description that
  // owns the option objects, and each page is a copy of the relevant ones.
  struct OptionSpec {
    const char* name;
    const char* help;
    bool takes_value;
  };

  // One line per command, and the only place either the `--command` hint or the top-level
  // listing is written from. `pu --help` cannot print the pages in full — it would be the
  // whole manual — but it can say what each command answers, which is the question a reader
  // arriving with no command has.
  struct CommandSpec {
    const char* name;
    const char* summary;
    // Off the top-level listing, which is a reader's map of what this tool is for. A command
    // that captures a provider's raw bytes answers a question about a gateway a reader has not
    // met yet, so it stays reachable and documented without being one of the visible choices.
    bool developer = false;
    // Printed above this command's own page, and nowhere else. A page is read by someone who
    // has already picked the command, so it is the one place where saying how it differs from
    // its nearest neighbour costs nothing and saves a wrong choice.
    const char* hint = nullptr;
  };

  const std::vector<CommandSpec> commands = {
      {"agents", "list the agents in agents.json and the gateway each one names"},
      {"ask", "send one prompt to an agent and print the answer"},
      {"chat", "hold a conversation: one turn per line, until you leave"},
      {"check", "read agents.json and report what it resolves to, without asking a provider"},
      {"delegate", "have another agent answer one question, in a turn of its own"},
      {"models", "list the models a gateway offers, or one agents.json entry for one of them"},
      {"probe",
       "capture a gateway's raw stream and summarise the fields it sent (developer tool)", true,
       "Reads agents.json for the gateway, its key and its model: what is asked of a provider "
       "that is already configured. `pu sse` takes a URL and headers instead, for a feed that "
       "is not a chat turn."},
      {"serve", "host the page and the API that a browser talks to"},
      {"sse",
       "read a server-sent-event stream and print each event", false,
       "A plain client: the URL, the method and the headers are yours, and no agents.json is "
       "read. To capture what a configured gateway sends, ask for one token, use `pu probe`."},
  };

  // The hint names what the listing names: a command kept off the page is not a name a reader
  // arriving with no command is asked to choose from.
  std::string command_hint = "command to execute (";
  bool first = true;
  for (const CommandSpec& command : commands) {
    if (command.developer) continue;
    command_hint += (first ? "" : "/") + std::string(command.name);
    first = false;
  }
  command_hint += ")";

  const std::vector<OptionSpec> global_options = {
      {"help,h", "show help message", false},
      {"version", "show version and exit", false},
      // `git -C` spellt the same way, and for the same reason: a command that reads a directory
      // can be run on one without leaving the one you are in. Every path the run touches
      // resolves from here, so nothing is read from one tree and written to another.
      {"directory,C", "run as if started in DIR", true},
      {"command", command_hint.c_str(), true},
  };

  const OptionSpec agent_option{"agent", "agent to use", true};
  const OptionSpec gateway_option{
      "gateway", "read this gateway's own host and key instead of the agent's backend", true};
  const OptionSpec model_option{
      "model", "model to use with --gateway; a gateway names a host, not a model", true};

  const std::map<std::string, std::vector<OptionSpec>> option_table = {
      // `--json` belongs to `agents` alone: `sse` prints JSON through `--format json`, because
      // a reader choosing a shape is one choice, not two flags that can disagree.
      {"agents", {{"json", "the shape GET /api/agents answers with, for a pipe", false}}},
      {"ask",
       {{"prompt", "prompt to send", true},
        agent_option,
        {"show-usage", "print the tokens and price this turn cost, on stderr", false}}},
      {"chat", {agent_option}},
      {"delegate",
       {{"agent", "the agent to ask", true},
        {"prompt", "the whole question", true},
        {"show-usage", "print the tokens and price this turn cost, on stderr", false}}},
      {"models",
       {agent_option, gateway_option, model_option,
        {"detail", "ids (default) for what you can pick, caps for the columns that decide "
                   "between two models",
         true},
        {"entry", "print an agents.json entry for this model id, ready to paste; implies "
                  "--detail entry",
         true},
        {"verify", "ask each listed model for one token and report what answered", false}}},
      {"probe",
       {agent_option, gateway_option, model_option,
        {"out", "directory the raw stream is written to (default ./provider-probes)", true},
        {"no-save", "print the summary without writing the raw stream", false},
        {"listing", "probe the model listing instead of the stream", false}}},
      {"check",
       {{"config", "agents.json to read (default: ./.pu/agents.json, then ~/.pu/)", true}}},
      {"sse",
       {{"url", "the endpoint to read (positional as well)", true},
        {"header", "a request header, repeatable: --header 'Authorization: Bearer ...'", true},
        {"post", "ask with POST instead of GET", false},
        {"body", "request body to send; '-' reads it from stdin", true},
        {"format", "text (default), json for one object per event, or raw for the bytes as sent",
         true},
        {"quiet", "in the text shape, print only the data without the event and id line", false}}},
      {"serve",
       {{"bind", "interface to listen on (default 127.0.0.1)", true},
        {"port", "port to listen on (default 8080)", true}}},
  };

  CommandLine line;

  auto declare = [](boost::program_options::options_description& into,
                    const OptionSpec& option) {
    // `--header` is given once per header, so it is a vector; every other value is one word.
    if (std::string(option.name) == "header") {
      into.add_options()(option.name,
                         boost::program_options::value<std::vector<std::string>>(),
                         option.help);
      return;
    }
    if (option.takes_value) {
      into.add_options()(option.name, boost::program_options::value<std::string>(), option.help);
    } else {
      into.add_options()(option.name, option.help);
    }
  };

  for (const OptionSpec& option : global_options) declare(line.all, option);

  // One declaration per name, however many commands read it: the set is what makes that true
  // rather than a comment hoping it is.
  std::set<std::string> declared;
  for (const OptionSpec& option : global_options) declared.insert(option.name);
  for (const auto& [command, options] : option_table) {
    (void)command;
    for (const OptionSpec& option : options) {
      if (declared.count(option.name) > 0) continue;
      declare(line.all, option);
      declared.insert(option.name);
    }
  }

  // A page is a copy of the option objects `all` declared, so the page and the parser cannot
  // disagree about a flag's wording. `--command` is the positional that named the command, so
  // a page leaves it out; --help and --version stay, since they are how a page is asked for.
  auto page = [&](const std::vector<OptionSpec>& own) {
    boost::program_options::options_description description("Global options");
    // A declared name may carry its short spelling — `directory,C` — and only the part before
    // the comma is the long name the option object answers to. Matching the whole string
    // would silently leave every such option off every page.
    auto copy = [&](const char* declared_name) {
      const std::string wanted =
          std::string(declared_name).substr(0, std::string(declared_name).find(','));
      for (const auto& option : line.all.options()) {
        if (option->long_name() == wanted) {
          description.add(option);
          return;
        }
      }
    };
    // --help is how this page was asked for, so it stays on one; --command is the
    // positional that named the command being described.
    copy("help");
    for (const OptionSpec& option : global_options) {
      if (std::string(option.name) != "command" && std::string(option.name) != "help,h") {
        copy(option.name);
      }
    }
    for (const OptionSpec& option : own) copy(option.name);
    return description;
  };

  // One page per name the table declares, so a command added to the table cannot be left
  // without one: a page that is missing does not fail, it falls back to `all`, and a reader
  // asking `pu <cmd> --help` would be shown every flag the line has.
  for (const auto& [command, options] : option_table) {
    line.per_command.emplace(command, page(options));
  }

  boost::program_options::positional_options_description positional;
  positional.add("command", 1);
  positional.add("prompt", 1);
  line.positional = std::move(positional);

  for (const CommandSpec& command : commands) {
    line.commands.push_back(
        {command.name, command.summary, command.developer, command.hint ? command.hint : ""});
  }
  return line;
}

const boost::program_options::options_description& CommandLine::HelpFor(
    const std::string& command) const {
  const auto it = per_command.find(command);
  return it == per_command.end() ? all : it->second;
}

std::string CommandLine::HintFor(const std::string& command) const {
  for (const CommandSummary& summary : commands) {
    if (summary.name == command) return summary.hint;
  }
  return "";
}

std::string CommandList(const CommandLine& line) {
  std::ostringstream printed;
  printed << "commands:\n";
  for (const CommandLine::CommandSummary& command : line.commands) {
    if (command.developer) continue;
    printed << "  " << command.name;
    printed << std::string(command.name.size() < 8 ? 8 - command.name.size() : 1, ' ');
    printed << command.summary << "\n";
  }
  printed << "\npu <command> --help names the flags that command takes.";
  return printed.str();
}

}  // namespace pu::cli
