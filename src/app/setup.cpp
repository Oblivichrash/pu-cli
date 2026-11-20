// SPDX-License-Identifier: GPL-3.0-only
// The first run: the questions a workspace with no agents.json has to answer before a turn
// can be sent, asked in the order a reader can answer them.
//
// It exists because the file it writes is the one thing `pu` cannot guess. A gateway, a host
// and a model are three facts about the reader's account that no default gets right, and the
// catalogue of models behind them is the one thing the reader usually does not know from
// memory -- so the list is fetched and shown rather than typed blind.
//
// Nothing here opens a session. The wizard finishes by writing the file; the command that
// asked for it then runs as it always did, reading the file it just found.

#include "pu/app/setup.hpp"

#include "pu/config/agents.hpp"
#include "pu/config/setup.hpp"
#include "pu/core/base.hpp"
#include "pu/core/beast_http_client.hpp"
#include "pu/core/platform.hpp"
#include "pu/llm/model_factory.hpp"
#include "pu/llm/model_listing.hpp"
#include "pu/model/gateway.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace pu::cli {

namespace {

std::string Trimmed(const std::string& text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

// One line of the reader's answer, with the default applied when they give none. Prompts go
// to stderr: an answer is the command's output, and a prompt on stdout would be read as part
// of it by a caller that captured the result.
//
// The default is shown in brackets after the question and the answer is read as a plain line:
// <enter> takes it, anything typed replaces it. No value is the reader leaving: nothing was
// typed, or the keyboard said stop. A wizard that took the default there would write a file
// they had just asked it not to write.
std::optional<std::string> Ask(const std::string& question, const std::string& fallback) {
  const auto [got_answer, answer] = platform::ReadLineWithDefault(question, fallback);
  if (!got_answer) return std::nullopt;
  const std::string trimmed = Trimmed(answer);
  return trimmed.empty() ? fallback : trimmed;
}

// The gateway, chosen by number or by name. The list is printed with its numbers because a
// first run has no reason to remember how a gateway spells itself, and a number is one
// keystroke.
std::optional<model::GatewayId> ParseGatewayAnswer(const std::string& answer,
                                                   const std::vector<model::GatewayId>& offered) {
  if (answer.empty()) return std::nullopt;
  bool digits = true;
  for (const char c : answer) {
    if (c < '0' || c > '9') {
      digits = false;
      break;
    }
  }
  if (digits && answer.size() <= 4) {
    const int index = std::stoi(answer);
    if (index >= 1 && static_cast<std::size_t>(index) <= offered.size()) {
      return offered[static_cast<std::size_t>(index) - 1];
    }
    return std::nullopt;
  }
  return model::ParseGatewayId(answer);
}

}  // namespace

std::optional<config::SetupChoice> RunSetupWizard(const std::filesystem::path& workspace) {
  // Only offered where there is something to write. A wizard whose answer would be refused at
  // the end is one that collected three facts to throw them away.
  const std::filesystem::path target = workspace / ".pu" / "agents.json";
  std::error_code ec;
  if (std::filesystem::exists(target, ec)) {
    std::cerr << "A configuration is already there: " << target.string() << "\n";
    return std::nullopt;
  }

  std::cerr << "No agents.json here yet. Three questions and this workspace is ready.\n\n";

  // The table is the list, in the order it is written: a gateway added to the table is one
  // the first run offers, and one the wizard leaves out by being written somewhere else is
  // one a reader has to know the name of before they can use it.
  std::vector<model::GatewayId> offered;
  {
    const std::span<const model::Gateway> all = model::Gateways();
    offered.reserve(all.size());
    for (const model::Gateway& gateway : all) offered.push_back(gateway.id);
  }
  if (offered.empty()) {
    std::cerr << "No gateways are built into this binary.\n";
    return std::nullopt;
  }

  std::cerr << "Which gateway?\n";
  for (std::size_t i = 0; i < offered.size(); ++i) {
    std::cerr << "  " << (i + 1) << ") " << model::GatewayName(offered[i]) << "\n";
  }
  std::cerr << "\n";

  // Either a number or a name is accepted, and the default is the first gateway's name: it
  // says which one <enter> takes, where a `1` would have described the list by a position
  // the reader then has to look up two lines above. The list is numbered so a number is
  // still one keystroke away.
  const std::optional<std::string> gateway_answer =
      Ask("Gateway", model::GatewayName(offered.front()));
  if (!gateway_answer) return std::nullopt;
  const std::optional<model::GatewayId> gateway = ParseGatewayAnswer(*gateway_answer, offered);
  if (!gateway) {
    std::cerr << "Not a gateway: " << *gateway_answer << ". One of: " << model::GatewayNames()
              << "\n";
    return std::nullopt;
  }

  config::SetupChoice choice;
  choice.gateway = *gateway;
  // Asked with the address the gateway itself would use as its default, so `<enter>` is an
  // answer the reader can see the result of: "blank for the gateway's own" names a value they
  // would have to remember from two questions ago, and a default that has to be remembered is
  // one that gets typed wrong.
  const std::optional<std::string> host = Ask("URL", model::DefaultHostFor(choice.gateway));
  if (!host) return std::nullopt;
  choice.host = *host;
  // Only asked where it is used. A product with no key has no reason to be shown a question
  // it must leave blank, and a blank answer there would look like a mistake rather than the
  // correct one.
  const std::string_view env_name = model::ApiKeyEnvFor(choice.gateway);
  if (!env_name.empty()) {
    const std::optional<std::string> key =
        Ask("API key (or leave blank to read " + std::string(env_name) + ")", std::string());
    if (!key) return std::nullopt;
    choice.api_key = *key;
  }

  // The catalogue, read with what has been answered so far. A listing that fails is not fatal:
  // a model whose id the reader knows can still be typed, and a wizard that stopped at the
  // first network error would be unusable on a machine behind a proxy.
  model::Backend probe;
  probe.gateway = choice.gateway;
  probe.host = choice.host.empty() ? model::DefaultHostFor(choice.gateway) : choice.host;
  // A blank answer means "use the variable the question named", so the listing asks with that
  // key: a wizard that could not list the catalogue for a key the environment already holds
  // would send the reader back to typing an id they came here to avoid.
  if (!choice.api_key.empty()) {
    probe.api_key = choice.api_key;
  } else if (!env_name.empty()) {
    if (const char* from_env = std::getenv(std::string(env_name).c_str());
        from_env != nullptr && *from_env != '\0') {
      probe.api_key = std::string(from_env);
    }
  }

  std::vector<model::ModelEntry> listed;
  try {
    http::BeastHttpClient http;
    listed = llm::ListModels(probe, http);
  } catch (const std::exception& e) {
    std::cerr << "\nCould not list the models: " << e.what() << "\n";
    std::cerr << "Type a model id instead.\n\n";
  }

  if (!listed.empty()) {
    std::cerr << "\nWhat this gateway offers:\n";
    // The number is what the answer is typed as, so the columns beside it have to clear it.
    // Measured against the last index rather than assumed: a catalogue of a hundred and six
    // entries is one more digit than one of ninety-nine.
    const std::size_t number_width = std::to_string(listed.size()).size();
    std::size_t id_width = 0;
    for (const model::ModelEntry& entry : listed) id_width = std::max(id_width, entry.id.size());

    for (std::size_t i = 0; i < listed.size(); ++i) {
      std::cerr << "  " << std::setw(static_cast<int>(number_width)) << (i + 1) << ") "
                << std::left << std::setw(static_cast<int>(id_width)) << listed[i].id
                << std::right << "  ";
      // What the catalogue says about the model, on the same line so a reader comparing two of
      // them does not have to hold one in their head. A model nothing is known about says so,
      // because a blank cell reads as a column that failed to print.
      const std::string caps = llm::CapabilityLine(listed[i].caps);
      std::cerr << (caps.empty() ? std::string("(nothing stated)") : caps) << "\n";
    }
    std::cerr << "\n";
  }

  // The model, asked until there is one. Unlike the questions above, this one has no default
  // to fall back on: a turn needs a model, and a guessed id would fail at the first request
  // rather than here. A blank line is therefore not an answer, it is the question again --
  // ending the wizard there would throw away the three answers already given, which is what
  // a reader coming from a failed listing (the case where there is nothing to pick from)
  // is most likely to do.
  //
  // Where the catalogue was listed, a number picks from it and a bare `<enter>` is still
  // blank -- the numbers are on screen, and a default that silently picked the first of
  // thirty models is a choice the reader did not make.
  const std::string question =
      listed.empty() ? std::string("Model id") : std::string("Model (number or id)");
  for (;;) {
    const std::optional<std::string> model_prompt = Ask(question, std::string());
    if (!model_prompt) return std::nullopt;
    const std::string& model_answer = *model_prompt;
    if (Trimmed(model_answer).empty()) {
      std::cerr << "A model is needed: "
                << (listed.empty() ? "type the id your gateway knows it by.\n"
                                   : "type its number, or its id.\n");
      continue;
    }
    bool digits = true;
    for (const char c : model_answer) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
    }
    if (digits && model_answer.size() <= 4) {
      const int index = std::stoi(model_answer);
      if (index >= 1 && static_cast<std::size_t>(index) <= listed.size()) {
        choice.model = listed[static_cast<std::size_t>(index) - 1].id;
        break;
      }
      // A number that names nothing in a list is a slip, not a model id: a model whose id is
      // all digits is not a thing any catalogue here lists, and accepting it would write a
      // file whose first request fails.
      if (!listed.empty()) {
        std::cerr << "No model numbered " << index << "; pick one from the list.\n";
        continue;
      }
    }
    // Not a number, or a number with no list to index: taken as an id, which is what the
    // reader typed and what the file is going to say.
    choice.model = model_answer;
    break;
  }

  try {
    config::ValidateSetupChoice(choice);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return std::nullopt;
  }

  return choice;
}

}  // namespace pu::cli
