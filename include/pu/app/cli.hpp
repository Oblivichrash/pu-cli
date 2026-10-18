// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/program_options.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace pu {

class Runtime;

namespace cli {

// The exit status of a run, as one contract for every subcommand (docs/design/
// cli-agent-workflow.md §3.1). They are separated by what a caller should do next:
//
//   kOk            nothing to do
//   kUsage         the invocation or the configuration is wrong — fix it, retrying cannot
//   kProvider      the provider refused or could not be reached — retry may work
//   kSandbox       the sandbox refused — ask a human, or change the policy
//   kInterrupted   a person stopped it — neither retry nor fix
//
// Anything else the process returns (a `serve` that cannot bind, say) keeps its old
// meaning until it is given one of these.
enum ExitCode : int {
  kOk = 0,
  kUsage = 1,
  kProvider = 2,
  kSandbox = 3,
  kInterrupted = 4,
};

// The whole command line, assembled in one place so that a name registered twice
// is caught where a test can drive it rather than at the process boundary.
//
// `all` is what the parser is given: every option of every subcommand, because which one was
// named is only known after parsing. `for_command` is what `pu <cmd> --help` prints, and what
// a wrong flag is judged against once the command is known.
struct CommandLine {
  // options_description holds const members and is not assignable, so both are constructed
  // with the title they print and filled where the line is assembled.
  boost::program_options::options_description all{"pu options"};
  boost::program_options::options_description global{"Global options"};
  // One page per subcommand, keyed by the name that selects it. Empty for a name this build
  // does not have.
  std::map<std::string, boost::program_options::options_description> per_command;
  boost::program_options::positional_options_description positional;

  // The page a subcommand's flags belong to, or the whole line for a name this build does not
  // know. Never empty, so a caller can always print something.
  const boost::program_options::options_description& HelpFor(const std::string& command) const;
};

CommandLine BuildCommandLine();

// Loads the configuration, runs `body`, and saves the session — the frame every command that
// needs a Runtime runs inside. A failure to initialize is `kUsage`: the configuration is what
// is wrong. `agent` names one for this run only when it is not empty.
int WithInitializedRuntime(const std::string& agent, Runtime& runtime,
                           const std::function<int()>& body);

int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime);
int RunChat(const std::string& agent, Runtime& runtime);
// `gateway` names a product to read without an agents.json entry: its host and key come from
// the gateway table, and neither the config nor the session is touched.
int RunProbe(const std::string& agent, const std::string& gateway, const std::string& model,
             const std::string& prompt, const std::string& out_dir, bool save, bool listing,
             Runtime& runtime);
int RunModels(const std::string& agent, const std::string& gateway, const std::string& model,
              bool verify, const std::string& entry_id, Runtime& runtime);
int RunServe(const std::string& host, std::uint16_t port, Runtime& runtime);
// What a config file resolves to, printed without asking anything of a provider. `--config`
// names a file outright; empty finds it the way every other command does. Exits 1 when the
// file cannot be loaded, 0 otherwise: a warning about a field this build ignores is not a
// failure, since a build may be older than the file.
int RunCheck(const std::string& config_path, Runtime& runtime);
// The agents a workspace configures, with the one the session names marked. `--json` prints
// the shape `GET /api/agents` answers with, from the same function.
int RunAgents(Runtime& runtime, bool as_json);

}  // namespace cli
}  // namespace pu
