// SPDX-License-Identifier: GPL-3.0-only
//
// `pu models` as the `pu` command frame sees it.
//
// The work is in `tools_cli/models.cpp`, which the standalone `pu-models` reaches too. What is
// left here is the part that belongs to `pu`: resolving a gateway from the running session's
// own table, which is the one thing a standalone binary has to do for itself.
//
#include "pu/app/cli.hpp"

#include <iostream>
#include <optional>
#include <string>

#include "pu/config/agents.hpp"
#include "pu/model/backend.hpp"
#include "pu/runtime/runtime.hpp"
#include "pu/tools_cli/models.hpp"

#include "app/cli_output.hpp"

namespace pu::cli {

namespace {

// Which gateway this run reads. A named one is built from the table and never adopted, so
// asking what a product offers leaves the session where it was; an unnamed one is the agent's
// own backend, which is the one a turn would use.
model::Backend BackendFor(const std::string& gateway, const std::string& model,
                          Runtime& runtime) {
  if (gateway.empty()) return runtime.CurrentBackend();
  model::Backend backend = model::BackendForGateway(gateway);
  if (!model.empty()) backend.model = model;
  return backend;
}

}  // namespace

int RunModels(const std::string& agent, const std::string& gateway, const std::string& model,
              ModelShape shape, const std::string& entry_id, bool verify, Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    tools_cli::ModelsOptions options;
    options.gateway = gateway;
    options.model = model;
    options.shape = static_cast<tools_cli::ModelShape>(shape);
    options.entry_id = entry_id;
    options.verify = verify;

    // The entry an `--entry` report copies is the file's spelling of one agent, which is why
    // the raw value is read rather than the parsed entry: the parsed one carries the key the
    // file only referred to by name.
    std::optional<tools_cli::AgentsSource> agents;
    if (shape == ModelShape::kEntry) {
      agents.emplace();
      agents->config_path = runtime.ConfigPath();
      agents->agent_name = runtime.ActiveAgent().name;
      agents->entry = config::FindRawAgentEntry(agents->config_path, agents->agent_name);
    }

    return static_cast<int>(tools_cli::RunModels(options, BackendFor(gateway, model, runtime),
                                                 runtime.Http(), runtime.SharedHttp(), agents));
  });
}

}  // namespace pu::cli
