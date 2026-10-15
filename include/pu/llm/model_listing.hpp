// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pu/core/http_client.hpp"

namespace pu {

namespace config {
struct BackendConfig;
}

}  // namespace pu

namespace pu::llm {

struct Capabilities {
  bool supports_tools = false;
  bool supports_images = false;
  bool supports_reasoning = false;
  // Measured: a request that carries no effort is answered 400 `11150`.
  bool only_reasoning = false;
  std::string default_effort;
  int max_input_tokens = 0;
};

struct ModelEntry {
  std::string id;
  Capabilities caps;
};

}  // namespace pu::llm

namespace pu::llm {

// The catalogue is on the chat base's origin, not under it: appending /v3/config to
// the chat base is answered 404.
inline std::string CodeBuddyCatalogUrl(const std::string& host) {
  constexpr std::string_view kVersionSegment = "/v2";
  std::string origin = host;
  if (origin.ends_with(kVersionSegment)) {
    origin.resize(origin.size() - kVersionSegment.size());
  }
  return origin + "/v3/config";
}

}  // namespace pu::llm

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

namespace pu::llm {

struct CatalogShape {
  std::string path{};
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
