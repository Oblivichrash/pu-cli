// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <boost/program_options.hpp>

#include "pu/core/http_client.hpp"
#include "pu/core/sse.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pu {

class Runtime;

namespace cli {

// The contract, and what a caller should do with each code, is in
// docs/ARCHITECTURE.md#what-a-command-exits-with-and-what-it-prints.
enum ExitCode : int {
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
  };
  std::vector<CommandSummary> commands;

  // Never empty: a name this build does not have gets the whole line.
  const boost::program_options::options_description& HelpFor(const std::string& command) const;
};

CommandLine BuildCommandLine();

std::string CommandList(const CommandLine& line);

// The frame every command that needs a Runtime runs inside; see
// docs/ARCHITECTURE.md#the-frame-every-command-runs-inside.
int WithInitializedRuntime(const std::string& agent, Runtime& runtime,
                           const std::function<int()>& body);

// `show_usage` moves what the turn cost onto stderr, off by default so that stdout stays the
// answer alone.
int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime,
           bool show_usage = false);
int RunChat(const std::string& agent, Runtime& runtime);
// `gateway` reads a product without an agents.json entry; `caps` prints the columns that
// decide between two models that both work. Both are in
// docs/ARCHITECTURE.md#the-frame-every-command-runs-inside.
int RunProbe(const std::string& agent, const std::string& gateway, const std::string& model,
             const std::string& prompt, const std::string& out_dir, bool save, bool listing,
             Runtime& runtime);
// What `models` prints, and how much of it. The three shapes are exclusive answers to the same
// listing, so they are one choice rather than three flags that could be set together: `kIds`
// answers "what can I pick", `kCaps` "which of these two", and `kEntry` "how do I write this
// one into agents.json". `verify` is separate because it is the only shape that asks a
// provider instead of reading a catalogue.
enum class ModelShape { kIds, kCaps, kEntry };

int RunModels(const std::string& agent, const std::string& gateway, const std::string& model,
              ModelShape shape, const std::string& entry_id, bool verify, Runtime& runtime);
int RunServe(const std::string& bind_address, std::uint16_t port, Runtime& runtime);
// docs/configuration.md#reading-it-back-pu-check
int RunCheck(const std::string& config_path);
// `as_json` prints the shape `GET /api/agents` answers with, from the same function.
int RunAgents(Runtime& runtime, bool as_json);

// What `sse` prints. The three shapes are exclusive: raw bytes, one JSON object per event, or
// the event read as text -- so they are one choice rather than three flags that could be set
// together and contradict each other.
enum class SseFormat { kText, kJson, kRaw };

struct SseOptions {
  std::string url;
  pu::http::HttpMethod method = pu::http::HttpMethod::kGet;
  std::vector<std::string> headers;
  std::optional<std::string> body;
  SseFormat format = SseFormat::kText;
  // Whether the `event:` and `id:` lines are printed beside the data in the text shape.
  bool quiet = false;
};

int RunSse(const SseOptions& options, Runtime& runtime);

// docs/ARCHITECTURE.md#the-frame-every-command-runs-inside
std::string FormatSseEvent(const core::SseEvent& event, const SseOptions& options);

}  // namespace cli
}  // namespace pu
