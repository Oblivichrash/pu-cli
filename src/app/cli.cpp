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
#include "pu/config/backend.hpp"
#include "pu/core/platform.hpp"
#include "pu/runtime/command_router.hpp"
#include "pu/llm/model_listing.hpp"
#include "pu/llm/probe.hpp"
#include "pu/runtime/runtime.hpp"

namespace pu::cli {

namespace {

void PrintChatHelp(Runtime& runtime) { std::cout << runtime.HelpText() << "\n"; }

// Notices are diagnostics, so they go to stderr: stdout carries what was asked for and
// nothing else, which is what keeps `pu ask "..." | ...` usable.
void PrintNotice(const ExecutionResult& result) {
  if (result.notice.empty()) return;
  if (result.was_streamed) std::cerr << "\n";
  std::cerr << result.notice << "\n";
}

// What the turn cost, for a caller that asked. Written on stderr with the other diagnostics:
// stdout is the answer, and a number beside it would make the answer the second field of a
// record nobody agreed to.
void PrintUsage(const ExecutionResult& result) {
  if (!result.usage) return;
  const TokenUsage& usage = *result.usage;

  std::ostringstream line;
  line << "usage: " << usage.prompt_tokens << " prompt, " << usage.completion_tokens
       << " completion";
  if (usage.cached_tokens > 0) line << ", " << usage.cached_tokens << " cached";
  if (usage.total_tokens > 0) line << ", " << usage.total_tokens << " total";
  // A price only where the product reported one, and named in that product's unit: two
  // products' units are not comparable, so the unit is printed rather than assumed.
  if (usage.cost) {
    line << ", " << usage.cost->amount;
    if (!usage.cost->unit.empty()) line << " " << usage.cost->unit;
  }
  if (result.rounds > 1) line << " over " << result.rounds << " provider calls";
  std::cerr << line.str() << "\n";
}

// The failed turn's message, printed once. The executor records it rather than logging it,
// so the front end decides whether it is a line here or a field somewhere else.
void PrintFailure(const ExecutionResult& result) {
  if (!result.has_error) return;
  std::cerr << (result.error_message.empty() ? "Request failed" : result.error_message) << "\n";
}

// The exit status a finished turn answers with. A command the router answered is not a
// provider failure: `pu ask "/help"` did what it was asked.
int ExitCodeForAsk(const ExecutionResult& result, bool is_command) {
  // A person stopped the process, which is neither a failure to retry nor one to fix. The
  // token is global, so it is asked rather than threaded through every result.
  if (platform::ShutdownRequested()) return kInterrupted;
  if (is_command) return kOk;
  // The sandbox refused everything the turn tried. Retrying cannot help, but what has to
  // change differs: a command or a policy for one, a front end that cannot ask for the
  // other.
  if (result.only_unaskable_calls) return kUsage;
  if (result.only_blocked_calls) return kSandbox;
  if (!result.has_error) return kOk;
  return kProvider;
}

// The backend a read-only command reads. Naming a gateway builds one from the table and
// never adopts it, so `pu models --gateway nim` leaves the session where it was.
config::Backend BackendFor(const std::string& gateway, const std::string& model,
                           Runtime& runtime) {
  if (gateway.empty()) return runtime.CurrentBackend();
  config::Backend backend = config::BackendForGateway(gateway);
  if (!model.empty()) backend.model = model;
  return backend;
}

}  // namespace

int WithInitializedRuntime(const std::string& agent, Runtime& runtime,
                           const std::function<int()>& body) {
  try {
    if (!agent.empty()) runtime.SetDefaultAgent(agent);
    runtime.Initialize();
    const int status = body();
    runtime.Shutdown();
    return status;
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return kUsage;
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

  const std::vector<OptionSpec> global_options = {
      {"help,h", "show help message", false},
      {"version", "show version and exit", false},
      {"command", "command to execute (agents/ask/chat/check/models/probe/serve/sse)", true},
  };

  const OptionSpec agent_option{"agent", "agent to use", true};
  const OptionSpec gateway_option{
      "gateway", "read this gateway's own host and key instead of the agent's backend", true};
  const OptionSpec model_option{
      "model", "model to use with --gateway; a gateway names a host, not a model", true};

  const std::map<std::string, std::vector<OptionSpec>> option_table = {
      // `--json` is shared: one JSON object per event for `sse`, the API's shape for
      // `agents`. The help text names both, since the flag is declared once.
      {"agents", {{"json", "one JSON object per line: an event, or the shape GET /api/agents "
                           "answers with",
                   false}}},
      {"ask",
       {{"prompt", "prompt to send", true},
        agent_option,
        {"show-usage", "print the tokens and price this turn cost, on stderr", false}}},
      {"chat", {agent_option}},
      {"models",
       {agent_option, gateway_option, model_option,
        {"verify", "ask each listed model for one token and report what answered", false},
        {"entry", "print an agents.json entry for this model id, ready to paste", true},
        {"caps", "print the capabilities and price that decide between two models", false}}},
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
        {"raw", "print the bytes as they arrive, unparsed", false},
        {"json", "print one JSON object per event", false},
        {"quiet", "print only the data, without the event and id line", false}}},
      {"serve",
       {{"host", "bind address (default 127.0.0.1)", true},
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
  auto page = [&](const std::string& command, const std::vector<OptionSpec>& own) {
    boost::program_options::options_description description("Global options");
    auto copy = [&](const char* name) {
      for (const auto& option : line.all.options()) {
        if (option->long_name() == std::string(name)) {
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

  line.per_command.emplace("agents", page("agents", option_table.at("agents")));
  line.per_command.emplace("ask", page("ask", option_table.at("ask")));
  line.per_command.emplace("chat", page("chat", option_table.at("chat")));
  line.per_command.emplace("models", page("models", option_table.at("models")));
  line.per_command.emplace("probe", page("probe", option_table.at("probe")));
  line.per_command.emplace("check", page("check", option_table.at("check")));
  line.per_command.emplace("sse", page("sse", option_table.at("sse")));
  line.per_command.emplace("serve", page("serve", option_table.at("serve")));

  boost::program_options::positional_options_description positional;
  positional.add("command", 1);
  positional.add("prompt", 1);
  line.positional = std::move(positional);
  return line;
}

const boost::program_options::options_description& CommandLine::HelpFor(
    const std::string& command) const {
  const auto it = per_command.find(command);
  return it == per_command.end() ? all : it->second;
}

int RunAsk(const std::string& agent, const std::string& prompt, Runtime& runtime,
           bool show_usage) {
  return WithInitializedRuntime(agent, runtime, [&] {
    bool is_command = false;
    ExecutionResult result =
        runtime.ProcessInput(prompt, is_command, platform::ShutdownToken(), nullptr, {}, nullptr,
                             platform::ShutdownToken());
    PrintFailure(result);
    if (!result.has_error) {
      if (result.was_streamed) {
        std::cout << "\n";
      } else if (!result.content.empty()) {
        std::cout << result.content << "\n";
      } else if (!is_command) {
        std::cout << "\n";
      }
    }
    PrintNotice(result);
    if (show_usage) PrintUsage(result);
    return ExitCodeForAsk(result, is_command);
  });
}

int RunChat(const std::string& agent, Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    const config::AgentEntry& active = runtime.ActiveAgent();
    std::string agent_info = "Connected to agent: " + active.name;
    if (!active.description.empty()) agent_info += " (" + active.description + ")";
    spdlog::info("{}", agent_info);
    spdlog::info("Type /help for available commands.");

    std::string input;
    while (std::cout << "> " << std::flush, std::getline(std::cin, input)) {
      if (input.empty()) continue;

      if (input == "/exit" || input == "/quit") break;

      try {
        bool is_command = false;
        ExecutionResult result =
            runtime.ProcessInput(input, is_command, platform::ShutdownToken(), nullptr, {},
                                 nullptr, platform::ShutdownToken());
        if (result.has_error) {
          if (is_command && result.error_message.empty()) {
            std::cout << "Unknown command. ";
            PrintChatHelp(runtime);
          } else {
            PrintFailure(result);
          }
        } else if (!result.was_streamed) {
          if (!result.content.empty()) {
            std::cout << result.content << "\n";
          } else if (!is_command) {
            std::cout << "\n";
          }
        }

        PrintNotice(result);
      } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
      }
    }
    std::cout << "\nGoodbye!\n";
    return kOk;
  });
}

// One probe run: a single-token prompt to the agent's backend, the raw stream kept,
// and the field names the gateway actually sent read back out of it. A ResponseShape
// for a new gateway is written from this capture rather than from a specification.
int RunProbe(const std::string& agent, const std::string& gateway, const std::string& model,
             const std::string& prompt, const std::string& out_dir, bool save, bool listing,
             Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    const config::Backend backend = BackendFor(gateway, model, runtime);
    // A catalogue read names no model; a turn does, and a gateway alone cannot say which.
    if (!listing && backend.model.empty()) {
      throw GenericError("--gateway names a host, not a model: pass --model, or --listing to "
                         "read the catalogue it offers");
    }

    if (listing) {
      // A listing is one answer rather than a stream, so there is nothing to keep as raw
      // bytes beyond the body the client already returned.
      const llm::ListingReport report = llm::ProbeListing(backend, runtime.Http());
      std::cout << "\n" << llm::FormatListingReport(report) << "\n";
      return report.error.empty() ? kOk : kProvider;
    }

    std::optional<std::string> raw_path;
    if (save) {
      std::filesystem::create_directories(out_dir);
      raw_path = (std::filesystem::path(out_dir) /
                  (std::string(config::GatewayName(backend.gateway)) + ".raw"))
                     .string();
      std::cout << "raw stream: " << *raw_path << "\n";
    }

    const llm::ProbeReport report = llm::ProbeStream(backend, runtime.Http(), prompt, raw_path);
    std::cout << "\n" << llm::FormatProbeReport(report) << "\n";
    // A capture that never parsed is a provider failure; one that parsed and merely lacks
    // fields is the finding the probe was run for, so it succeeded.
    return report.stream_parsed ? kOk : kProvider;
  });
}

int RunModels(const std::string& agent, const std::string& gateway, const std::string& model,
              bool verify, const std::string& entry_id, bool caps, Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    const config::Backend backend = BackendFor(gateway, model, runtime);
    const std::vector<llm::ModelEntry> models = config::ListModels(backend, runtime.Http());

    if (!entry_id.empty()) {
      // A model outside the listing is exactly the one being written for.
      llm::ModelEntry wanted{entry_id, {}};
      for (const llm::ModelEntry& model : models) {
        if (model.id == entry_id) {
          wanted = model;
          break;
        }
      }
      const std::optional<boost::json::value> source =
          config::FindRawAgentEntry(runtime.ConfigPath(), runtime.ActiveAgent().name);
      if (!source)
        throw GenericError("Cannot read the entry to copy from " + runtime.ConfigPath());
      std::cout << config::FormatAgentEntry(wanted, *source) << "\n";
      return kOk;
    }

    if (!verify) {
      std::cout << llm::FormatModelList(
                       models, config::GatewayName(backend.gateway), backend.host, backend.model,
                       caps ? llm::ListingDetail::kCaps : llm::ListingDetail::kIds)
                << "\n";
      return kOk;
    }

    const std::vector<llm::ModelCheck> checks =
        llm::VerifyModels(backend, models, runtime.SharedHttp());
    int answered = 0;
    for (const llm::ModelCheck& check : checks) {
      std::cout << check.model << "  ";
      if (check.answered) {
        std::cout << "ok";
        ++answered;
      } else {
        std::cout << "unavailable: " << check.detail;
      }
      std::cout << "\n";
    }
    std::cout << answered << " of " << checks.size() << " model(s) answered\n";
    // A verification that found a model down is the answer the caller asked for, and a
    // script needs to read it without parsing the text.
    return answered == static_cast<int>(checks.size()) ? kOk : kProvider;
  });
}

}  // namespace pu::cli
