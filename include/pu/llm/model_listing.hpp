// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/core/http_client.hpp"
#include "pu/llm/capabilities.hpp"
#include "pu/llm/model_lister.hpp"

namespace pu::llm {

// Where a listing keeps what it says about a model, and what the names are. The paths a
// host puts its catalogue on differ, so this is data rather than a branch on the type.
struct CatalogShape {
  std::string path;
  std::string container;
  // The field a model's id sits under, for a one-level listing: container[].id_key.
  std::string id_key;
  // Where a second level nests, for a listing shaped container[].nested[]: CodeBuddy's
  // agent list. Empty for a one-level listing.
  std::string nested;
  // What the host calls each capability. An empty name means this host does not report
  // it, which leaves that capability unknown rather than false.
  std::string supports_tools;
  std::string supports_images;
  std::string supports_reasoning;
  std::string only_reasoning;
  // The effort a model carries, as a one-level path: parent and the field under it.
  std::string effort_parent;
  std::string effort_field;
  std::string max_input_tokens;
};

// An Ollama host's listing, read from `{host}/api/tags`, where a model's id sits under
// `name` rather than `id`.
class OllamaModelLister final : public pu::ModelLister {
 public:
  std::vector<llm::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;
};

// An OpenAI-compatible host's listing, read from `{host}/models`.
class OpenAIModelLister final : public pu::ModelLister {
 public:
  std::vector<llm::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;
};

// The CodeBuddy gateway's catalogue, read from `{origin}/v3/config` under the client's own
// header set, because `/models` is not a route there. Each agent's `models` list is the
// answer: `data.models[]` also names image models a turn cannot run, while the capability
// fields beside each entry are what a caller needs to decide what it may ask for.
class CodeBuddyModelLister final : public pu::ModelLister {
 public:
  std::vector<llm::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;
};

}  // namespace pu::llm
