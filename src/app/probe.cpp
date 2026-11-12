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

namespace {

// The backend a read-only command reads. Naming a gateway builds one from the table and
// never adopts it, so `pu probe --gateway nim` leaves the session where it was.
model::Backend BackendFor(const std::string& gateway, const std::string& model,
                          Runtime& runtime) {
  if (gateway.empty()) return runtime.CurrentBackend();
  model::Backend backend = model::BackendForGateway(gateway);
  if (!model.empty()) backend.model = model;
  return backend;
}

}  // namespace

// One probe run: a single-token prompt to the agent's backend, the raw stream kept,
// and the field names the gateway actually sent read back out of it. An adapter for a
// new gateway is written from this capture rather than from a specification.
int RunProbe(const std::string& agent, const std::string& gateway, const std::string& model,
             const std::string& prompt, const std::string& out_dir, bool save, bool listing,
             Runtime& runtime) {
  return WithInitializedRuntime(agent, runtime, [&] {
    tools_cli::ProbeOptions options;
    options.gateway = gateway;
    options.model = model;
    options.prompt = prompt;
    options.out_dir = out_dir;
    options.save = save;
    options.listing = listing;
    // The subcommand borrows the session's backend so `pu probe --gateway nim` picks up
    // the agent's keys; the standalone tool builds the same table from its own config read.
    return static_cast<int>(
        tools_cli::RunProbe(options, BackendFor(gateway, model, runtime), runtime.Http()));
  });
}
}  // namespace pu::cli
