// SPDX-License-Identifier: GPL-3.0-only
// The catalogue read itself. It names a gateway and prints what that gateway offers; it opens
// no session, writes no transcript, and adopts nothing -- asking what a product carries leaves
// the workspace where it was.

#include "pu/tools/models.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "pu/config/agents.hpp"
#include "pu/llm/model_check.hpp"
#include "pu/llm/model_factory.hpp"
#include "pu/llm/model_listing.hpp"

namespace pu::tools {

namespace {

// The entry an `kEntry` report describes: the listed model when the catalogue carries it, and
// one made of the id alone when it does not. A model nothing lists is exactly the one a row is
// being written for, so a missing id is an empty entry rather than an error.
model::ModelEntry PickEntry(const std::vector<model::ModelEntry>& models,
                            const std::string& entry_id) {
  for (const model::ModelEntry& listed : models) {
    if (listed.id == entry_id) return listed;
  }
  return model::ModelEntry{entry_id, {}};
}

}  // namespace

ExitCode RunModels(const ModelsOptions& options, const model::Backend& backend,
                   http::HttpClient& http, std::shared_ptr<http::HttpClient> shared_http,
                   const std::optional<AgentsSource>& agents) {  // The listing is read once for every shape. Two of them ask nothing else of the network, and
  // the third only adds a token request per model.
  const std::vector<model::ModelEntry> models = llm::ListModels(backend, http);

  if (options.shape == ModelShape::kEntry) {
    if (agents && agents->entry) {
      std::cout << config::FormatAgentEntry(PickEntry(models, options.entry_id), *agents->entry)
                << "\n";
      return ExitCode::kOk;
    }
    // Named as the file the entry would have come from, because a copy has to start somewhere
    // and the table is what says where.
    std::cerr << "cannot read the entry to copy from "
              << (agents ? agents->config_path : std::string("this workspace's table")) << "\n";
    return ExitCode::kUsage;
  }

  if (!options.verify) {
    std::cout << llm::FormatModelList(
                     models, model::GatewayName(backend.gateway), backend.host, backend.model,
                     options.shape == ModelShape::kCaps ? llm::ListingDetail::kCaps
                                                        : llm::ListingDetail::kIds)
              << "\n";
    return ExitCode::kOk;
  }

  const std::vector<llm::ModelCheck> checks = llm::VerifyModels(backend, models, shared_http);
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
  // A verification that found a model down is the answer the caller asked for, and a script
  // needs to read it without parsing the text.
  return answered == static_cast<int>(checks.size()) ? ExitCode::kOk : ExitCode::kProvider;
}

}  // namespace pu::tools
