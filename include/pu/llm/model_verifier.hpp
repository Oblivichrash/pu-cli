// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/config/backend.hpp"
#include "pu/core/http_client.hpp"
#include "pu/llm/capabilities.hpp"

namespace pu::llm {

// What a model answered when it was asked for one token, or why it was not asked.
// `answered` is the whole point: a listing says a model is named, not that the account
// may run it.
struct ModelCheck {
  std::string model;
  bool answered = false;
  // The gateway's own words when it refused, the reason no request went out when none
  // did, and empty when the model answered.
  std::string detail;
};

// Asks each model for a one-token reply and reports what came back, one entry per
// model, in the order given. A refusal is a check result and not a throw: the loop's
// job is to report every model, and a credential the host rejects fails all of them.
// A model the catalogue says is answered only with a reasoning level is not asked at all
// when the backend sends none, since that request cannot succeed.
std::vector<ModelCheck> VerifyModels(const config::BackendConfig& cfg,
                                     const std::vector<ModelEntry>& models,
                                     std::shared_ptr<http::HttpClient> http);

}  // namespace pu::llm
