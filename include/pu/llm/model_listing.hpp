// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/core/http_client.hpp"
#include "pu/llm/capabilities.hpp"
#include "pu/llm/model_lister.hpp"

namespace pu::llm {

struct CatalogShape {
  std::string path;
  std::string container;
  std::string id_key;
  std::string nested;
  // An empty name leaves that capability unknown.
  std::string supports_tools;
  std::string supports_images;
  std::string supports_reasoning;
  std::string only_reasoning;
  // One level: the parent object and the field under it.
  std::string effort_parent;
  std::string effort_field;
  std::string max_input_tokens;
};

class OllamaModelLister final : public pu::ModelLister {
 public:
  std::vector<llm::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;
};

class OpenAIModelLister final : public pu::ModelLister {
 public:
  std::vector<llm::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;
};

// Read from `{origin}/v3/config`; `/models` is not a route there.
class CodeBuddyModelLister final : public pu::ModelLister {
 public:
  std::vector<llm::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;
};

}  // namespace pu::llm
