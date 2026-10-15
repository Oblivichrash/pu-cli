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
  std::string nested{};
  std::string supports_tools{};
  std::string supports_images{};
  std::string supports_reasoning{};
  std::string only_reasoning{};
  // One level: the parent object and the field under it.
  std::string effort_parent{};
  std::string effort_field{};
  std::string max_input_tokens{};
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

// Read from `{origin}/v3/config`; `/models` is not a route there. The headers are the
// gate's, which is why they are handed in rather than named here.
class CodeBuddyModelLister final : public pu::ModelLister {
 public:
  explicit CodeBuddyModelLister(std::vector<std::string> headers = {});

  std::vector<llm::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;

 private:
  std::vector<std::string> headers_;
};

}  // namespace pu::llm
