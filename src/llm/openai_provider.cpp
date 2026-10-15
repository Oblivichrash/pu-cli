// SPDX-License-Identifier: GPL-3.0-only
#include "pu/llm/openai_provider.hpp"

namespace pu {

OpenAIProvider::OpenAIProvider(const Config& config, std::shared_ptr<pu::http::HttpClient> http)
    : OpenAIProtocol(config, std::move(http)), extra_headers_(config.extra_headers) {}

std::vector<std::string> OpenAIProvider::Headers() const {
  std::vector<std::string> headers = StreamingProvider::Headers();
  if (extra_headers_) {
    const std::vector<std::string> extra = extra_headers_();
    headers.insert(headers.end(), extra.begin(), extra.end());
  }
  return headers;
}

}  // namespace pu
