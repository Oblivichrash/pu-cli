// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pu/core/http_client.hpp"
#include "pu/model/listing.hpp"

namespace pu {

namespace model {
struct Backend;
}

}  // namespace pu

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

// How a gateway's catalogue is fetched once its origin is known: where the listing lives, and
// the shape its answer is read with. Exposed so a probe can fetch the same endpoint the lister
// does and diff the bytes against the same names, rather than restating either.
struct ListingTarget {
  std::string url;
  model::CatalogShape shape;
};

class ModelLister {
 public:
  virtual ~ModelLister() = default;

  virtual std::vector<model::ModelEntry> ListModels(const std::string& host,
                                                  const std::string& api_key,
                                                  http::HttpClient& http) const = 0;

  // The target this lister would read for `host`, or nothing for a lister that has no
  // catalogue to describe. A probe asks the lister rather than matching on a gateway's name,
  // so a gateway added with its own catalogue is probed the moment it is listed.
  virtual std::optional<model::ListingTarget> Target(const std::string& /*host*/) const {
    return std::nullopt;
  }
};

}  // namespace pu

// How much of a listing to print. `kIds` is the default and answers "what can I pick"; `kCaps`
// adds the columns that decide between two models that both work, which is the question a
// listing is consulted for after the first one. It is a parameter rather than a second
// renderer so the two answers cannot disagree about a model's price or its active marker.
namespace pu::llm {

enum class ListingDetail { kIds, kCaps };

// Renders a listing as both the `models` command and the `/models` command print it. An
// empty listing says so rather than printing a heading over nothing.
std::string FormatModelList(const std::vector<model::ModelEntry>& models,
                            std::string_view gateway,
                            std::string_view host,
                            std::string_view active_model,
                            ListingDetail detail = ListingDetail::kIds);

class OllamaModelLister final : public pu::ModelLister {
 public:
  std::vector<model::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;

  std::optional<model::ListingTarget> Target(const std::string& host) const override;
};

class OpenAIModelLister final : public pu::ModelLister {
 public:
  std::vector<model::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;

  std::optional<model::ListingTarget> Target(const std::string& host) const override;
};

// Read from `{origin}/v3/config`; `/models` is not a route there. The headers are the
// gateway's, which is why they are handed in rather than named here.
class CodeBuddyModelLister final : public pu::ModelLister {
 public:
  explicit CodeBuddyModelLister(std::vector<std::string> headers = {});

  std::vector<model::ModelEntry> ListModels(const std::string& host, const std::string& api_key,
                                          http::HttpClient& http) const override;

  std::optional<model::ListingTarget> Target(const std::string& host) const override;

 private:
  std::vector<std::string> headers_;
};

// The target a backend's own lister would read, taken from the gateway table rather than from
// the gateway's name: a product that brings its own catalogue is probed through the same
// declaration that lists it. Nothing comes back when the table names no lister for it.
std::optional<model::ListingTarget> ListingTargetForGateway(const model::Backend& backend);

}  // namespace pu::llm
