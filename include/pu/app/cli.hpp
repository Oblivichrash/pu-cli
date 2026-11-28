// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/program_options.hpp>

#include "pu/core/http_client.hpp"
#include "pu/core/sse.hpp"
#include "pu/tools/models.hpp"
#include "pu/tools/probe.hpp"
#include "pu/tools/sse.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pu {

class Runtime;

namespace cli {

// The contract, and what a caller should do with each code, is in
// docs/errors.md#what-a-command-exits-with-and-what-it-prints. Scoped because a second
// `ExitCode` names the same four values for the standalone tools, and both are visible to
// whatever includes both headers.
enum class ExitCode : int {
  kOk = 0,
  kUsage = 1,
  kProvider = 2,
  kSandbox = 3,
  kInterrupted = 4,
};

// `all` is what the parser is given — every option of every subcommand, because which one
// was named is only known after parsing. `per_command` is what `pu <cmd> --help` prints, and
// what a wrong flag is judged against once the command is known. Both
// `options_description`s are constructed with the title they print because their members are
// const and the type is not assignable.
struct CommandLine {
  boost::program_options::options_description all{"pu options"};
  boost::program_options::options_description global{"Global options"};
  std::map<std::string, boost::program_options::options_description> per_command;
  boost::program_options::positional_options_description positional;

  // Every command this build has, with the one line that says what it answers. The same
  // list writes the `--command` hint and the top-level page, so a new command is named in
  // both or in neither.
  struct CommandSummary {
    std::string name;
    std::string summary;
    // Named and documented, but not offered in the top-level listing; see `CommandSpec`.
    bool developer = false;
    // How this command differs from its nearest neighbour. Empty when there is nothing to
    // say, and printed only above this command's own page; see `CommandSpec`.
    std::string hint;
  };
  std::vector<CommandSummary> commands;

  // Never empty: a name this build does not have gets the whole line.
  const boost::program_options::options_description& HelpFor(const std::string& command) const;

  // The hint for a command's own page, or an empty string when it has none.
  std::string HintFor(const std::string& command) const;
};

CommandLine BuildCommandLine();

// The page a caller arrives at with no command chosen: the list of commands, then the flags
// every command answers. One function rather than two strings a caller concatenates, because
// the two places that show this page — `pu --help` and a bare `pu` — must not drift apart.
std::string TopLevelPage(const CommandLine& line);
// The frame every command that needs a Runtime runs inside; see
// docs/layout.md#the-tree. `ephemeral` settles it before
// Initialize, so it belongs to the command's signature rather than to its body.
int WithInitializedRuntime(const std::string& agent, Runtime& runtime,
                           const std::function<int()>& body, bool ephemeral = false);

// `show_usage` moves what the turn cost onto stderr, off by default so that stdout stays the
// answer alone.
// `attachments` are files to put on the message beside the prompt, in the order they were named.
// A path that cannot be read is a mistake in the invocation rather than a turn to send, so it is
// refused before anything reaches a provider.
int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime,
           bool show_usage = false, const std::vector<std::string>& attachments = {});
int RunChat(const std::string& agent, Runtime& runtime);
// A developer tool, kept off the top-level listing: it captures a provider's raw bytes for
// someone writing a gateway adapter, which is not a question `agents.json` has an answer to.
// `gateway` reads a product without an agents.json entry; `listing` reads the catalogue where
// `models` reads and prints it, and is kept for the raw report rather than the ids.
int RunProbe(const std::string& agent, const std::string& gateway, const std::string& model,
             const std::string& prompt, const std::string& out_dir, bool save, bool listing,
             Runtime& runtime);
// What `models` prints, and how much of it. The three shapes are exclusive answers to the same
// listing, so they are one choice rather than three flags that could be set together: `kIds`
// answers "what can I pick", `kCaps` "which of these two", and `kEntry` "how do I write this
// one into agents.json". `verify` is separate because it is the only shape that asks a
// provider instead of reading a catalogue. Declared in one place because `pu models` and the
// standalone `pu-models` call the one implementation.
using ModelShape = pu::tools::ModelShape;

// The backend a read-only command reads — `models` and `probe`, the two that ask a product
// something without taking a turn. A named gateway is built from the table and never adopted,
// so asking what one offers leaves the session where it was; an unnamed one is the agent's
// own backend, which is the one a turn would use. Stated once because it was written twice,
// byte for byte, and two answers to "which gateway did this read" is the drift that only
// shows up when they disagree.
model::Backend ReadOnlyBackend(const std::string& gateway, const std::string& model,
                               Runtime& runtime);

int RunModels(const std::string& agent, const std::string& gateway, const std::string& model,
              ModelShape shape, const std::string& entry_id, bool verify, Runtime& runtime);
int RunServe(const std::string& bind_address, std::uint16_t port, Runtime& runtime);
// docs/configuration.md#reading-it-back-pu-check
// `workspace` is the directory the run is about — `-C` when one was given, the current
// directory otherwise. It is passed rather than read here because a sandbox root is relative
// to it and so is the search for an unset table, and a check that reported one directory
// while the run used another would be the split `-C` exists to remove.
int RunCheck(const std::string& config_path, const std::filesystem::path& workspace);

// One turn taken by another agent, along the path a tool takes: the question is the whole of
// what that turn knows, because the agent asked has no idea what this conversation has said.
int RunDelegate(const std::string& agent, const std::string& prompt, Runtime& runtime,
                bool show_usage);
// `as_json` prints the shape `GET /api/agents` answers with, from the same function.
int RunAgents(Runtime& runtime, bool as_json);

// What `sse` prints, and the shape `pu sse` and the standalone `pu-sse` both read. Declared
// in one place because the two binaries call the one implementation.
using SseFormat = pu::tools::SseFormat;
using SseOptions = pu::tools::SseOptions;

int RunSse(const SseOptions& options, http::HttpClient& http);

// docs/layout.md#the-tree
std::string FormatSseEvent(const core::SseEvent& event, const SseOptions& options);

// Everything `main` does once it has the argv a process was started with. The entry point
// itself holds only the two things that must happen before any command: the signal handler
// and the gateway table. Kept here rather than in `main.cpp` because the parse, the page a
// wrong flag is answered with and the dispatch are one decision — which command this line
// is about — and a reader looking for what `pu <cmd>` runs should not have to open the file
// that starts the process.
int RunCommandLine(int argc, char* argv[]);

}  // namespace cli
}  // namespace pu
