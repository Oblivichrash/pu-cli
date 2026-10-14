// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/config/backend.hpp"
#include "pu/core/http_client.hpp"

namespace pu::llm {

// What a model answered when it was asked for one token. `answered` is the whole
// point: a listing says a model is named, not that the account may run it.
struct ModelCheck {
  std::string model;
  bool answered = false;
  std::string detail;  // the gateway's own words when it refused, empty when it did not
};

// Asks each model for a one-token reply and reports what came back, one entry per
// model, in the order given. A refusal is a check result and not a throw: the loop's
// job is to report every model, and a credential the host rejects fails all of them.
std::vector<ModelCheck> VerifyModels(const config::BackendConfig& cfg,
                                     const std::vector<std::string>& models,
                                     std::shared_ptr<http::HttpClient> http);

}  // namespace pu::llm
