// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>
#include <string_view>

namespace pu::llm {

// The gateway's model catalogue does not hang off the chat base: a turn goes to
// {base}/chat/completions, while the catalogue is at /v3/config on the same origin, with
// the chat base's version segment replaced. Both were measured; appending /v3/config to
// the chat base answers 404.
inline std::string CodeBuddyCatalogUrl(const std::string& host) {
  constexpr std::string_view kVersionSegment = "/v2";
  std::string origin = host;
  if (origin.ends_with(kVersionSegment)) {
    origin.resize(origin.size() - kVersionSegment.size());
  }
  return origin + "/v3/config";
}

}  // namespace pu::llm
