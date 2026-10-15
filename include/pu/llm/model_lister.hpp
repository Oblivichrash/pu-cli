// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pu/core/http_client.hpp"
#include "pu/llm/capabilities.hpp"

namespace pu {

class LLMProvider;

class ModelLister {
 public:
  virtual ~ModelLister() = default;

  virtual std::vector<llm::ModelEntry> ListModels(const std::string& host,
                                                  const std::string& api_key,
                                                  http::HttpClient& http) const = 0;
};

namespace llm {

// Renders a listing as both the `models` command and the `/models` command print it. An
// empty listing says so rather than printing a heading over nothing.
std::string FormatModelList(const std::vector<ModelEntry>& models,
                            std::string_view gateway,
                            std::string_view host,
                            std::string_view active_model);

}  // namespace llm

}  // namespace pu
