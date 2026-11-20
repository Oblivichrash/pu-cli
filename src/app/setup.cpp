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
#include "pu/llm/model_factory.hpp"
#include "pu/model/gateway.hpp"

#include <cstdlib>
#include <iostream>
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
std::string Ask(const std::string& question, const std::string& fallback) {
  std::cerr << question;
  if (!fallback.empty()) std::cerr << " [" << fallback << "]";
  std::cerr << ": ";
  std::cerr.flush();

  std::string answer;
  if (!std::getline(std::cin, answer)) return fallback;
  answer = Trimmed(answer);
  return answer.empty() ? fallback : answer;
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

  const std::vector<model::GatewayId> offered{
      model::GatewayId::kOllama, model::GatewayId::kOpenAI, model::GatewayId::kAnthropic,
      model::GatewayId::kCodeBuddy};

  std::cerr << "Which gateway?\n";
  for (std::size_t i = 0; i < offered.size(); ++i) {
    std::cerr << "  " << (i + 1) << ") " << model::GatewayName(offered[i]);
    // A local product needs no account, which is the fact that decides the choice for a
    // first run on a machine with nothing configured yet.
    if (model::ApiKeyEnvFor(offered[i]).empty()) std::cerr << "  (no key needed)";
    std::cerr << "\n";
  }
  std::cerr << "\n";

  const std::string gateway_answer = Ask("Gateway", std::string(model::GatewayName(offered[0])));
  const std::optional<model::GatewayId> gateway = ParseGatewayAnswer(gateway_answer, offered);
  if (!gateway) {
    std::cerr << "Not a gateway: " << gateway_answer << ". One of: " << model::GatewayNames()
              << "\n";
    return std::nullopt;
  }

  config::SetupChoice choice;
  choice.gateway = *gateway;
  choice.host = Ask("URL (blank for the gateway's own)", std::string());
  // Only asked where it is used. A product with no key has no reason to be shown a question
  // it must leave blank, and a blank answer there would look like a mistake rather than the
  // correct one.
  const std::string_view env_name = model::ApiKeyEnvFor(choice.gateway);
  if (!env_name.empty()) {
    choice.api_key = Ask("API key (or leave blank to read " + std::string(env_name) + ")",
                         std::string());
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
    for (std::size_t i = 0; i < listed.size(); ++i) {
      std::cerr << "  " << (i + 1) << ") " << listed[i].id << "\n";
    }
    std::cerr << "\n";
  }

  const std::string model_answer = Ask("Model (number or id)", std::string());
  bool digits = !model_answer.empty();
  for (const char c : model_answer) {
    if (c < '0' || c > '9') {
      digits = false;
      break;
    }
  }
  if (!model_answer.empty() && digits && model_answer.size() <= 4) {
    const int index = std::stoi(model_answer);
    if (index >= 1 && static_cast<std::size_t>(index) <= listed.size()) {
      choice.model = listed[static_cast<std::size_t>(index) - 1].id;
    }
  }
  if (choice.model.empty()) {
    // Not a number, or a number past the end of a list that failed to arrive: taken as an id,
    // which is what the reader typed and what the file is going to say.
    choice.model = model_answer;
  }
  if (Trimmed(choice.model).empty()) {
    std::cerr << "No model, so there is nothing to write.\n";
    return std::nullopt;
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
