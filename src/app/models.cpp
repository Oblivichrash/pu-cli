// SPDX-License-Identifier: GPL-3.0-only
//
// `pu models`: what a gateway offers, in one of three shapes. Read-only in the sense that
// matters — it names a gateway from the table and never adopts it, so asking what a product
// offers leaves the session where it was.
//
#include "pu/app/cli.hpp"

#include <iostream>
#include <string>
#include <vector>

#include "pu/llm/model_check.hpp"
#include "pu/llm/model_factory.hpp"
#include "pu/llm/model_listing.hpp"
#include "pu/model/backend.hpp"
#include "pu/runtime/runtime.hpp"

#include "app/cli_output.hpp"

namespace pu::cli {

namespace {

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
    const model::Backend backend = BackendFor(gateway, model, runtime);
    const std::vector<model::ModelEntry> models = llm::ListModels(backend, runtime.Http());

    // The listing is read once for every shape. Two of them ask nothing else of the network,
    // and the third only adds a token request per model.
    if (shape == ModelShape::kEntry) {
      // A model outside the listing is exactly the one being written for, so a missing id is
      // an empty entry rather than an error.
      model::ModelEntry wanted{entry_id, {}};
      for (const model::ModelEntry& listed : models) {
        if (listed.id == entry_id) {
          wanted = listed;
          break;
        }
      }
      const std::optional<boost::json::value> source =
          config::FindRawAgentEntry(runtime.ConfigPath(), runtime.ActiveAgent().name);
      if (!source) throw GenericError("Cannot read the entry to copy from " + runtime.ConfigPath());
      std::cout << config::FormatAgentEntry(wanted, *source) << "\n";
      return static_cast<int>(ExitCode::kOk);
    }

    if (!verify) {
      std::cout << llm::FormatModelList(
                       models, model::GatewayName(backend.gateway), backend.host, backend.model,
                       shape == ModelShape::kCaps ? llm::ListingDetail::kCaps
                                                  : llm::ListingDetail::kIds)
                << "\n";
      return static_cast<int>(ExitCode::kOk);
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
    return answered == static_cast<int>(checks.size()) ? static_cast<int>(ExitCode::kOk)
                                                       : static_cast<int>(ExitCode::kProvider);
  });
}
}  // namespace pu::cli
