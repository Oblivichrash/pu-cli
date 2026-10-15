// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
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

}  // namespace pu
