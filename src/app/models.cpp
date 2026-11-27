// SPDX-License-Identifier: GPL-3.0-only
//
// `pu models` as the `pu` command frame sees it.
//
// The work is in `tools/models.cpp`, which the standalone `pu-models` reaches too. What is
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
#include "pu/tools/models.hpp"

#include "app/cli_output.hpp"

namespace pu::cli {

int RunModels(const std::string& agent, const std::string& gateway, const std::string& model,
              ModelShape shape, const std::string& entry_id, bool verify, Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    tools::ModelsOptions options;
    options.gateway = gateway;
    options.model = model;
    options.shape = static_cast<tools::ModelShape>(shape);
    options.entry_id = entry_id;
    options.verify = verify;

    // The entry an `--entry` report copies is the file's spelling of one agent, which is why
    // the raw value is read rather than the parsed entry: the parsed one carries the key the
    // file only referred to by name.
    std::optional<tools::AgentsSource> agents;
    if (shape == ModelShape::kEntry) {
      agents.emplace();
      agents->config_path = runtime.ConfigPath();
      agents->agent_name = runtime.ActiveAgent().name;
      agents->entry = config::FindRawAgentEntry(agents->config_path, agents->agent_name);
    }

    return static_cast<int>(tools::RunModels(
        options, ReadOnlyBackend(gateway, model, runtime),
                                                 runtime.Http(), runtime.SharedHttp(), agents));
  });
}

}  // namespace pu::cli
