// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <optional>
#include <string>

#include "pu/model/backend.hpp"

namespace pu::config {

// What a first run asks for, before it is written down.
//
// One agent, because that is what a first run is: a backend to talk to and a name to record
// the choice under. The name is not asked for -- it is the agent the session will say it is
// using, and the file is written in the single-agent form where the name has no counterpart
// to sit beside. `agent_name` defaults to "default", which is what a reader who has no
// opinion would type anyway; a caller who has one sets it.
struct SetupChoice {
  model::GatewayId gateway = model::GatewayId::kOllama;
  std::string host;
  std::string model;
  // Empty for a backend that answers without one: a local Ollama has no key, and a file
  // written with `api_key: ""` would ask a product for a key it never had.
  std::string api_key;
  std::string agent_name = "default";
};

// The two questions the wizard cannot answer for the reader: which agent this is, and what
// the endpoint is. Everything else is a default that a first run can accept.
//
// `host` is the address the reader typed. Empty means "the gateway's own default", which is
// what `DefaultHostFor` answers and is the right answer for every product whose address does
// not differ per customer.
//
// `api_key` is checked against the gateway rather than accepted blindly: a product that
// authenticates needs one, and a first run that wrote a file without it would fail on the
// first turn with a message about a header instead of about the missing key.
//
// Throws `GenericError` naming the field that is wrong. A wizard that collected an answer
// and then wrote an unusable file is worse than one that refused the answer while the reader
// was still looking at the question.
void ValidateSetupChoice(const SetupChoice& choice);

// The agents.json this choice becomes, as text, in the single-agent form: `name`, `backend`
// and nothing else. The key is written only when there is one, so a local product's file does
// not carry a field whose value is a lie about authentication.
//
// `PU_AGENTS_CONFIG` is not consulted: the caller decides where the file goes, because a
// wizard writing to an address the reader did not name is a file they will not find.
std::string RenderSetupConfig(const SetupChoice& choice);

// Where a first run's file belongs: `./.pu/agents.json`, which is what `FindConfigPath`
// reads first and what a workspace-local choice should be. The directory is created if it is
// missing; a file already there is refused rather than overwritten, because the run that
// asked for a wizard is a run that found no file, and reaching this with one means something
// else wrote it.
std::string WriteSetupConfig(const SetupChoice& choice, const std::string& workspace);

}  // namespace pu::config
