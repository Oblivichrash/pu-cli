// SPDX-License-Identifier: GPL-3.0-only
#pragma once

//
// Turning a backend into something that reaches a network.
//
// `Backend` is a description, and a description does not dial. These two functions do, which is
// why they live in `llm` rather than beside the structure: one builds the provider a turn goes
// through, the other reads the catalogue the provider's host publishes. Keeping them apart lets
// `model` be included by code that only formats a table, without that code pulling in a
// provider or an HTTP client.
//

#include <memory>
#include <string>
#include <vector>

#include "pu/model/backend.hpp"
#include "pu/model/listing.hpp"

namespace pu {
class LLMProvider;
namespace http {
class HttpClient;
}  // namespace http
}  // namespace pu

namespace pu::llm {

// The provider a backend names: which product it is decides which one is built, and the host
// and key it carries are what that provider dials.
std::unique_ptr<pu::LLMProvider> CreateBackend(const model::Backend& cfg,
                                               std::shared_ptr<pu::http::HttpClient> http);

// Reads the model listing the backend's host reports, with what the host says about each
// model. The gateway answers where that listing lives. A listing that names no model is an
// empty vector, not an error. Throws HttpError on a failed read.
std::vector<model::ModelEntry> ListModels(const model::Backend& cfg, pu::http::HttpClient& http);

}  // namespace pu::llm
