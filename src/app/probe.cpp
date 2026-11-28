// SPDX-License-Identifier: GPL-3.0-only
//
// `pu probe`: a developer tool kept off the top-level listing. It captures a provider's raw
// bytes for someone writing a gateway adapter, which is not a question `agents.json` has an
// answer to.
//
#include "pu/app/cli.hpp"

#include <string>

#include "pu/model/backend.hpp"
#include "pu/runtime/runtime.hpp"

#include "app/cli_output.hpp"

namespace pu::cli {

// One probe run: a single-token prompt to the agent's backend, the raw stream kept,
// and the field names the gateway actually sent read back out of it. An adapter for a
// new gateway is written from this capture rather than from a specification.
int RunProbe(const std::string& agent, const std::string& gateway, const std::string& model,
             const std::string& prompt, const std::string& out_dir, bool save, bool listing,
             Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    tools::ProbeOptions options;
    options.gateway = gateway;
    options.model = model;
    options.prompt = prompt;
    options.out_dir = out_dir;
    options.save = save;
    options.listing = listing;
    // The subcommand borrows the session's backend so `pu probe --gateway nim` picks up
    // the agent's keys; the standalone tool builds the same table from its own config read.
    return static_cast<int>(
        tools::RunProbe(options, ReadOnlyBackend(gateway, model, runtime), runtime.Http()));
  });
}
}  // namespace pu::cli
