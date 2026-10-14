// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <vector>

#include "pu/core/http_client.hpp"
#include "pu/llm/model_lister.hpp"

namespace pu::llm {

// An Ollama host's listing, read from `{host}/api/tags`, where a model's id sits under
// `name` rather than `id`.
class OllamaModelLister final : public pu::ModelLister {
 public:
  std::vector<std::string> ListModels(const std::string& host, const std::string& api_key,
                                      http::HttpClient& http) const override;
};

// An OpenAI-compatible host's listing, read from `{host}/models`.
class OpenAIModelLister final : public pu::ModelLister {
 public:
  std::vector<std::string> ListModels(const std::string& host, const std::string& api_key,
                                      http::HttpClient& http) const override;
};

// The CodeBuddy gateway's catalogue, read from `{host}/v3/config` under the client's own
// header set, because `/models` is not a route there. Each agent's `models` list is the
// answer: `data.models[]` also names image models a turn cannot run.
class CodeBuddyModelLister final : public pu::ModelLister {
 public:
  std::vector<std::string> ListModels(const std::string& host, const std::string& api_key,
                                      http::HttpClient& http) const override;
};

}  // namespace pu::llm
