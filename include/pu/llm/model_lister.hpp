// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "pu/core/http_client.hpp"
#include "pu/llm/capabilities.hpp"

namespace pu {

class LLMProvider;

// The listing a backend type's host reports. A host does not end in a slash; the path
// is appended verbatim, as with the chat endpoints.
class ModelLister {
 public:
  virtual ~ModelLister() = default;

  // The models the host names, each with whatever it said about them. A listing that
  // names none is an empty vector, not an error; a failed read throws HttpError, as the
  // client does.
  virtual std::vector<llm::ModelEntry> ListModels(const std::string& host,
                                                  const std::string& api_key,
                                                  http::HttpClient& http) const = 0;
};

}  // namespace pu
