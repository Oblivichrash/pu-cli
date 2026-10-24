// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "pu/llm/openai_protocol.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pu {

// One gateway's backend in the OpenAI protocol: that protocol at the gateway's host, with the
// headers the gateway sends. Everything the wire format does belongs to OpenAIProtocol; what
// is left here is which host and which headers, which is what a gateway states. The `openai`
// and `codebuddy` gateways build this, and so does every agent of the `openai` gateway — OpenAI,
// DeepSeek, NVIDIA and a vLLM of one's own differ only in host, headers and response shape.
class OpenAIProvider final : public OpenAIProtocol {
 public:
  using Config = OpenAIProtocol::Config;

  explicit OpenAIProvider(const Config& config, std::shared_ptr<pu::http::HttpClient> http);

 protected:
  std::vector<std::string> Headers() const override;

 private:
  std::function<std::vector<std::string>()> extra_headers_;
};

}  // namespace pu
