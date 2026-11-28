// SPDX-License-Identifier: GPL-3.0-only
// One catalogue read, in the layer that needs no session. `pu models` and `pu-models` both come
// through here, so a shape one of them prints is a shape the other prints.

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <boost/json.hpp>

#include "pu/core/http_client.hpp"
#include "pu/model/backend.hpp"
#include "pu/tools/exit_code.hpp"

namespace pu::tools {

// What `models` prints, and how much of it. The three shapes are exclusive answers to the same
// listing, so they are one choice rather than three flags that could be set together: `kIds`
// answers "what can I pick", `kCaps` "which of these two", and `kEntry` "how do I write this one
// into agents.json". `verify` is separate because it is the only shape that asks a provider
// instead of reading a catalogue.
enum class ModelShape { kIds, kCaps, kEntry };

struct ModelsOptions {
  // Empty: the backend handed in is used as it stands, which is what a caller that already
  // resolved one wants.
  std::string gateway;
  std::string model;
  ModelShape shape = ModelShape::kIds;
  // Which model an `kEntry` report describes. A model outside the listing is the one being
  // written for, so it is an empty entry rather than an error.
  std::string entry_id;
  bool verify = false;
};

// The raw agent table an `kEntry` report copies from. Only that shape reads it: the entry it
// prints is the file's spelling of one agent with the listing's model swapped in, and a parsed
// entry would carry the expanded key rather than the declared one. `entry` is empty when the
// table cannot be read, which the shape treats as an error.
struct AgentsSource {
  std::string config_path;
  std::string agent_name;
  std::optional<boost::json::value> entry;
};

// The parsed agent table an `kEntry` report needs, read the same way whichever binary asks. It
// is a free function rather than a member so a standalone caller can build one without a
// session: the file is the whole of what it takes.
AgentsSource ReadAgentsSource(const std::string& config_path, const std::string& agent_name);

// Runs one catalogue read and prints it. The backend is already resolved, and `http` is the
// only thing this needs from the process: which agent named the gateway, or whether a session
// is open, are not this function's concern. `agents` is read only by the `kEntry` shape;
// `shared_http` is for the verification, which a provider built per model outlives.
ExitCode RunModels(const ModelsOptions& options, const model::Backend& backend,
                   http::HttpClient& http, std::shared_ptr<http::HttpClient> shared_http,
                   const std::optional<AgentsSource>& agents);

}  // namespace pu::tools
